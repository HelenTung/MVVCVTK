#include "Host/MetrologyReport.h"
#include "AlignmentVoxelVolumeData.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using Status = MetrologyReportWriteStatus;
struct Failure final {
  Status status;
  const char *message;
};
void Require(bool value, const char *message) {
  if (!value)
    throw Failure{Status::InvalidInput, message};
}
class Sink final {
public:
  Sink(std::ostream &stream, const MetrologyReportWriteOptions &options)
      : options(options), stream(stream) {}
  void Check() const {
    if (options.isCancelled && options.isCancelled())
      throw Failure{Status::Cancelled, "Report write cancelled."};
  }
  void Put(std::string_view text) {
    Check();
    const auto limit =
        options.maxBytes.value_or(std::numeric_limits<std::size_t>::max());
    if (text.size() > limit - confirmed - pending)
      throw Failure{Status::BudgetExceeded,
                    "Report output exceeds byte budget."};
    while (!text.empty()) {
      Check();
      const auto count = std::min(text.size(), buffer.size() - pending);
      std::copy_n(text.data(), count, buffer.data() + pending);
      pending += count;
      text.remove_prefix(count);
      if (pending == buffer.size())
        Flush();
    }
  }
  void Flush() {
    Check();
    if (!stream.good() || !stream.rdbuf())
      throw Failure{Status::IoError, "Report output stream is unavailable."};
    if (!pending)
      return;
    std::streamsize written = 0;
    try {
      written = stream.rdbuf()->sputn(buffer.data(),
                                      static_cast<std::streamsize>(pending));
    } catch (...) {
      throw Failure{Status::IoError,
                    "Report output stream threw while writing."};
    }
    if (written < 0 || written > static_cast<std::streamsize>(pending)) {
      stream.setstate(std::ios::badbit);
      throw Failure{Status::IoError,
                    "Report stream returned an invalid byte count."};
    }
    if (written > 0)
      confirmed += static_cast<std::size_t>(written);
    const bool complete = written == static_cast<std::streamsize>(pending);
    pending = 0;
    if (!complete) {
      stream.setstate(std::ios::badbit);
      throw Failure{Status::IoError, "Report output stream rejected bytes."};
    }
  }
  std::size_t confirmed = 0;
  const MetrologyReportWriteOptions &options;

private:
  std::ostream &stream;
  std::array<char, 16384> buffer{};
  std::size_t pending = 0;
};
std::size_t Utf8(std::string_view s, std::size_t i) {
  const auto first = static_cast<unsigned char>(s[i]);
  if (first < 128)
    return 1;
  unsigned count = 0, code = 0, minimum = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    count = 2;
    code = first & 31;
    minimum = 0x80;
  } else if (first >= 0xE0 && first <= 0xEF) {
    count = 3;
    code = first & 15;
    minimum = 0x800;
  } else if (first >= 0xF0 && first <= 0xF4) {
    count = 4;
    code = first & 7;
    minimum = 0x10000;
  } else
    throw Failure{Status::InvalidInput, "Report text is not valid UTF-8."};
  Require(s.size() - i >= count, "Truncated UTF-8 report text.");
  for (unsigned j = 1; j < count; ++j) {
    const auto c = static_cast<unsigned char>(s[i + j]);
    Require((c & 0xC0) == 0x80, "Invalid UTF-8 continuation.");
    code = (code << 6) | (c & 63);
  }
  Require(code >= minimum && code <= 0x10FFFF &&
              !(code >= 0xD800 && code <= 0xDFFF),
          "Invalid UTF-8 code point.");
  return count;
}
template <class T> void Number(Sink &sink, T value) {
  std::array<char, 128> text{};
  const auto converted = [&] {
    if constexpr (std::is_floating_point_v<T>)
      return std::to_chars(text.data(), text.data() + text.size(), value,
                           std::chars_format::general,
                           std::numeric_limits<T>::max_digits10);
    else
      return std::to_chars(text.data(), text.data() + text.size(), value);
  }();
  Require(converted.ec == std::errc{}, "Report number conversion failed.");
  sink.Put(
      {text.data(), static_cast<std::size_t>(converted.ptr - text.data())});
}
std::string Ref(const DataRevisionRef &ref) {
  static constexpr char hex[] = "0123456789abcdef";
  std::array<char, 54> text{};
  std::size_t i = 0;
  for (auto b : ref.entityId.bytes) {
    text[i++] = hex[b >> 4];
    text[i++] = hex[b & 15];
  }
  text[i++] = ':';
  const auto end =
      std::to_chars(text.data() + i, text.data() + text.size(), ref.generation);
  Require(end.ec == std::errc{}, "Report revision conversion failed.");
  return {text.data(), end.ptr};
}
template <class T>
const char *Enum(T value, std::initializer_list<const char *> names) {
  const auto index = static_cast<std::size_t>(value);
  Require(index < names.size(), "Unknown report enum value.");
  return names.begin()[index];
}
const char *Name(VoxelVolumeMethod v) {
  return Enum(v, {"StoredValueRange", "Labels"});
}
const char *Name(VoxelVolumeValidity v) {
  return Enum(v, {"Complete", "IncompleteSamples", "TouchesBoundary",
                  "EmptySelection"});
}
const char *Name(AlignmentUnit v) {
  return Enum(v, {"ModelUnit", "Millimeter", "Meter"});
}
const char *Name(AlignmentGeometryKind v) {
  return Enum(v, {"Point", "Line", "Plane", "Circle", "Sphere", "Cylinder"});
}
const char *Name(AlignmentMethod v) {
  return Enum(
      v, {"SequentialPlanes", "PlaneTwoHoles", "Rps", "ConstrainedBestFit"});
}
const char *Name(AlignmentAssociation v) {
  return Enum(v, {"LeastSquares", "SequentialLeastSquares", "Contact"});
}
const char *Name(AlignmentConstraintKind v) {
  return Enum(v, {"Hard", "Soft", "Check"});
}
const char *Name(AlignmentStatus v) {
  return Enum(v, {"FullyDetermined", "Underconstrained", "Degenerate",
                  "Conflicting", "Ambiguous", "NotConverged", "Cancelled",
                  "DeadlineExceeded", "Stale", "InvalidInput", "Unsupported",
                  "BudgetExceeded", "Unavailable", "InternalError", "IoError"});
}
const char *Name(AlignmentDeviationMetric v) {
  return Enum(v, {"UnsignedDistance", "NominalNormalProjection"});
}
const char *Name(AlignmentDeviationState v) {
  return Enum(v, {"NotSelected", "Valid", "InvalidSource", "InvalidTarget",
                  "TooFar", "NormalMismatch", "NormalUnavailable"});
}
const char *Name(AlignmentInspectionVerdict v) {
  return Enum(v, {"NotEvaluated", "Pass", "Fail", "Indeterminate"});
}
const char *Name(InspectionGeometrySource v) {
  return Enum(v, {"MeasuredSurface", "ReferenceSurface", "NominalDefinition"});
}
const char *Name(InspectionMetric v) {
  return Enum(v, {"Radius", "Diameter", "CenterX", "CenterY", "CenterZ",
                  "CenterDistance", "OrientedDirectionAngleDegrees",
                  "SignedParallelPlaneDistance", "FitRms", "FitMaximum"});
}
const char *Name(ImageValueType v) {
  return Enum(v, {"Unknown", "Int8", "UInt8", "Int16", "UInt16", "Int32",
                  "UInt32", "Int64", "UInt64", "Float32", "Float64"});
}
const char *Name(ImageSourceKind v) {
  return Enum(v, {"RawFile", "TiffSeries", "Memory"});
}

template <class T, class Enable = void> struct Codec;
class Json final {
public:
  explicit Json(Sink &sink) : sink(sink) {}
  template <class T> void Value(const T &v) { Codec<T>::Write(*this, v); }
  template <class T> void Field(std::string_view key, const T &v) {
    Key(key);
    Value(v);
  }
  void Object() {
    Before();
    sink.Put("{");
    Push(false);
  }
  void Array() {
    Before();
    sink.Put("[");
    Push(true);
  }
  void End() {
    Require(depth && !stack[depth - 1].needsValue,
            "Invalid report JSON structure.");
    sink.Put(stack[--depth].array ? "]" : "}");
  }
  void Null() {
    Before();
    sink.Put("null");
  }
  void Boolean(bool v) {
    Before();
    sink.Put(v ? "true" : "false");
  }
  template <class T> void Numeric(T v) {
    if constexpr (std::is_floating_point_v<T>)
      if (!std::isfinite(v)) {
        Object();
        Field("numericState", std::string(std::isnan(v) ? "NaN"
                                          : v > 0       ? "PositiveInfinity"
                                                        : "NegativeInfinity"));
        End();
        return;
      }
    Before();
    Number(sink, v);
  }
  void Text(std::string_view v) {
    Before();
    Quoted(v);
  }
  Sink &sink;

private:
  struct Scope {
    bool array = false, first = true, needsValue = false;
  };
  std::array<Scope, 64> stack{};
  std::size_t depth = 0;
  bool root = false;
  void Push(bool array) {
    Require(depth < stack.size(), "Report JSON nesting exceeds schema limit.");
    stack[depth++] = {array, true, false};
  }
  void Before() {
    if (!depth) {
      Require(!root, "Duplicate JSON root.");
      root = true;
      return;
    }
    auto &scope = stack[depth - 1];
    if (scope.array) {
      if (!scope.first)
        sink.Put(",");
      scope.first = false;
    } else {
      Require(scope.needsValue, "JSON object value lacks a key.");
      scope.needsValue = false;
    }
  }
  void Key(std::string_view key) {
    Require(depth && !stack[depth - 1].array && !stack[depth - 1].needsValue,
            "Invalid report JSON key.");
    auto &scope = stack[depth - 1];
    if (!scope.first)
      sink.Put(",");
    scope.first = false;
    Quoted(key);
    sink.Put(":");
    scope.needsValue = true;
  }
  void Quoted(std::string_view text) {
    sink.Put("\"");
    std::size_t begin = 0;
    for (std::size_t i = 0; i < text.size();) {
      if ((i - begin) >= 4096) {
        sink.Put(text.substr(begin, i - begin));
        begin = i;
      }
      sink.Check();
      const auto c = static_cast<unsigned char>(text[i]);
      if (c == '"' || c == '\\' || c < 32) {
        sink.Put(text.substr(begin, i - begin));
        if (c == '"' || c == '\\') {
          const char escaped[]{'\\', static_cast<char>(c)};
          sink.Put({escaped, 2});
        } else {
          static constexpr char hex[] = "0123456789abcdef";
          const char escaped[]{'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
          sink.Put({escaped, 6});
        }
        ++i;
        begin = i;
      } else
        i += Utf8(text, i);
    }
    sink.Put(text.substr(begin));
    sink.Put("\"");
  }
};
template <class T> struct Codec<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
  static void Write(Json &j, T v) {
    if constexpr (std::is_same_v<T, bool>)
      j.Boolean(v);
    else
      j.Numeric(v);
  }
};
template <class T> struct Codec<T, std::enable_if_t<std::is_enum_v<T>>> {
  static void Write(Json &j, T v) { j.Text(Name(v)); }
};
template <> struct Codec<std::string> {
  static void Write(Json &j, const std::string &v) { j.Text(v); }
};
template <class T> struct Codec<std::optional<T>> {
  static void Write(Json &j, const std::optional<T> &v) {
    if (v)
      j.Value(*v);
    else
      j.Null();
  }
};
template <class T> struct Codec<std::shared_ptr<T>> {
  static void Write(Json &j, const std::shared_ptr<T> &v) {
    if (v)
      j.Value(*v);
    else
      j.Null();
  }
};
template <class T> struct Codec<std::vector<T>> {
  static void Write(Json &j, const std::vector<T> &v) {
    j.Array();
    for (const auto &x : v)
      j.Value(x);
    j.End();
  }
};
template <class T, std::size_t N> struct Codec<std::array<T, N>> {
  static void Write(Json &j, const std::array<T, N> &v) {
    j.Array();
    for (const auto &x : v)
      j.Value(x);
    j.End();
  }
};
template <class... T> struct Codec<std::variant<T...>> {
  static void Write(Json &j, const std::variant<T...> &v) {
    std::visit([&](const auto &x) { j.Value(x); }, v);
  }
};
template <> struct Codec<DataRevisionRef> {
  static void Write(Json &j, const DataRevisionRef &v) { j.Text(Ref(v)); }
};
template <> struct Codec<DataTypeId> {
  static void Write(Json &j, const DataTypeId &v) {
    j.Object();
    j.Field("name", v.name);
    j.Field("schemaVersion", v.schemaVersion);
    j.End();
  }
};
template <> struct Codec<DataInputRef> {
  static void Write(Json &j, const DataInputRef &v) {
    j.Object();
    j.Field("role", v.role);
    j.Field("source", v.source);
    j.End();
  }
};
template <> struct Codec<DataProvenance> {
  static void Write(Json &j, const DataProvenance &v) {
    j.Object();
    j.Field("producerId", v.producerId);
    j.Field("operationId", v.operationId);
    j.Field("algorithmVersion", v.algorithmVersion);
    j.Field("canonicalParameters", v.canonicalParameters);
    j.End();
  }
};
template <> struct Codec<GridGeometry3D> {
  static void Write(Json &j, const GridGeometry3D &v) {
    j.Object();
    j.Field("extent", v.extent);
    j.Field("dimensions", v.dimensions);
    j.Field("spacing", v.spacing);
    j.Field("origin", v.origin);
    j.Field("direction", v.direction);
    j.Field("coordinateFrame", v.coordinateFrame);
    j.End();
  }
};
template <> struct Codec<DatasetIdentity> {
  static void Write(Json &j, const DatasetIdentity &v) {
    j.Object();
    j.Field("datasetId", v.datasetId);
    j.Field("inspectionId", v.inspectionId);
    j.Field("objectId", v.objectId);
    j.Field("batchId", v.batchId);
    j.End();
  }
};
template <> struct Codec<ImageSourceMetadata> {
  static void Write(Json &j, const ImageSourceMetadata &v) {
    j.Object();
    j.Field("kind", v.kind);
    j.Field("uri", v.uri);
    j.Field("byteSize", v.byteSize);
    j.Field("digest", v.digest);
    j.End();
  }
};
template <> struct Codec<ScalarSemantics> {
  static void Write(Json &j, const ScalarSemantics &v) {
    j.Object();
    j.Field("quantity", v.quantity);
    j.Field("unit", v.unit);
    j.Field("slope", v.slope);
    j.Field("intercept", v.intercept);
    j.Field("noData", v.noData);
    j.End();
  }
};
template <> struct Codec<MetadataAttribute> {
  static void Write(Json &j, const MetadataAttribute &v) {
    j.Object();
    j.Field("key", v.key);
    j.Field("variantIndex", v.value.index());
    j.Field("value", v.value);
    j.End();
  }
};
template <> struct Codec<ImageMetadata> {
  static void Write(Json &j, const ImageMetadata &v) {
    j.Object();
    j.Field("identity", v.identity);
    j.Field("source", v.source);
    j.Field("scalar", v.scalar);
    j.Field("attributes", v.attributes);
    j.End();
  }
};
template <> struct Codec<AlignmentInput> {
  static void Write(Json &j, const AlignmentInput &v) {
    j.Object();
    j.Field("source", v.source);
    j.Field("mesh", v.mesh);
    j.Field("scopeData", v.scopeData);
    j.Field("sourceFrameId", v.sourceFrameId);
    j.Field("coordinateFrame", v.coordinateFrame);
    j.Field("scope", v.scope);
    j.Field("unit", v.unit);
    j.Field("sourceBinding", v.sourceBinding);
    j.Field("meshBinding", v.meshBinding);
    j.Field("scopeBinding", v.scopeBinding);
    j.End();
  }
};
template <> struct Codec<AlignmentRegion> {
  static void Write(Json &j, const AlignmentRegion &v) {
    j.Object();
    j.Field("pinnedMesh", v.pinnedMesh);
    j.Field("vertexIds", v.vertexIds);
    j.Field("targetBounds", v.targetBounds);
    j.End();
  }
};
template <> struct Codec<AlignmentGeometrySpec> {
  static void Write(Json &j, const AlignmentGeometrySpec &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("kind", v.kind);
    j.Field("association", v.association);
    j.Field("region", v.region);
    j.Field("targetDirection", v.targetDirection);
    j.Field("radiusRange", v.radiusRange);
    j.Field("maxFitRms", v.maxFitRms);
    j.Field("minCoverage", v.minCoverage);
    j.Field("minDirectionCosine", v.minDirectionCosine);
    j.End();
  }
};
template <> struct Codec<AlignmentConstraint> {
  static void Write(Json &j, const AlignmentConstraint &v) {
    j.Object();
    j.Field("geometryIndex", v.geometryIndex);
    j.Field("nominalPoint", v.nominalPoint);
    j.Field("targetDirection", v.targetDirection);
    j.Field("kind", v.kind);
    j.Field("priority", v.priority);
    j.Field("weight", v.weight);
    j.Field("tolerance", v.tolerance);
    j.Field("sectionPlane", v.sectionPlane);
    j.End();
  }
};
template <> struct Codec<AlignmentFitPair> {
  static void Write(Json &j, const AlignmentFitPair &v) {
    j.Object();
    j.Field("vertexId", v.vertexId);
    j.Field("nominalPoint", v.nominalPoint);
    j.Field("targetNormal", v.targetNormal);
    j.Field("weight", v.weight);
    j.End();
  }
};
template <> struct Codec<AlignmentSurfaceScale> {
  static void Write(Json &j, const AlignmentSurfaceScale &v) {
    j.Object();
    j.Field("voxelSize", v.voxelSize);
    j.Field("maxPairDistance", v.maxPairDistance);
    j.Field("iterationLimit", v.iterationLimit);
    j.End();
  }
};
template <> struct Codec<AlignmentSurfaceStage> {
  static void Write(Json &j, const AlignmentSurfaceStage &v) {
    j.Object();
    j.Field("voxelSize", v.voxelSize);
    j.Field("maxPairDistance", v.maxPairDistance);
    j.Field("selectedVertices", v.selectedVertices);
    j.Field("samples", v.samples);
    j.Field("iterations", v.iterations);
    j.Field("rejectedSamples", v.rejectedSamples);
    j.Field("selectedArea", v.selectedArea);
    j.Field("acceptedArea", v.acceptedArea);
    j.Field("isConverged", v.isConverged);
    j.End();
  }
};
template <> struct Codec<AlignmentSurfaceFit> {
  static void Write(Json &j, const AlignmentSurfaceFit &v) {
    j.Object();
    j.Field("sourceRegion", v.sourceRegion);
    j.Field("referenceTriangles", v.referenceTriangles);
    j.Field("iterationLimit", v.iterationLimit);
    j.Field("isPointToPlane", v.isPointToPlane);
    j.Field("scales", v.scales);
    j.End();
  }
};
template <> struct Codec<AlignmentPrealignParams> {
  static void Write(Json &j, const AlignmentPrealignParams &v) {
    j.Object();
    j.Field("sampleSpacing", v.sampleSpacing);
    j.Field("normalRadius", v.normalRadius);
    j.Field("featureRadius", v.featureRadius);
    j.Field("maxPairDistance", v.maxPairDistance);
    j.Field("minCoverage", v.minCoverage);
    j.Field("edgeLengthRatio", v.edgeLengthRatio);
    j.Field("candidateSeparation", v.candidateSeparation);
    j.Field("neighborLimit", v.neighborLimit);
    j.Field("matchCount", v.matchCount);
    j.Field("iterationLimit", v.iterationLimit);
    j.Field("candidateLimit", v.candidateLimit);
    j.Field("randomSeed", v.randomSeed);
    j.Field("sourceTriangles", v.sourceTriangles);
    j.Field("referenceTriangles", v.referenceTriangles);
    j.End();
  }
};
template <> struct Codec<AlignmentPrealignStats> {
  static void Write(Json &j, const AlignmentPrealignStats &v) {
    j.Object();
    j.Field("sourceSamples", v.sourceSamples);
    j.Field("referenceSamples", v.referenceSamples);
    j.Field("sourceFeatures", v.sourceFeatures);
    j.Field("referenceFeatures", v.referenceFeatures);
    j.Field("trials", v.trials);
    j.Field("candidates", v.candidates);
    j.Field("bestCoverage", v.bestCoverage);
    j.Field("sourceRejectedTriangles", v.sourceRejectedTriangles);
    j.Field("sourceSelectedArea", v.sourceSelectedArea);
    j.Field("sourceRejectedArea", v.sourceRejectedArea);
    j.Field("isCandidateLimitReached", v.isCandidateLimitReached);
    j.End();
  }
};
template <> struct Codec<AlignmentRecipe> {
  static void Write(Json &j, const AlignmentRecipe &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("targetFrameId", v.targetFrameId);
    j.Field("unit", v.unit);
    j.Field("method", v.method);
    j.Field("geometries", v.geometries);
    j.Field("constraints", v.constraints);
    j.Field("exactMesh", v.exactMesh);
    j.Field("fitPairs", v.fitPairs);
    j.Field("surfaceFit", v.surfaceFit);
    j.Field("prealignment", v.prealignment);
    j.Field("nominalData", v.nominalData);
    j.Field("datumCount", v.datumCount);
    j.Field("datumOffsets", v.datumOffsets);
    j.Field("lengthScale", v.lengthScale);
    j.Field("rankTolerance", v.rankTolerance);
    j.Field("conditionLimit", v.conditionLimit);
    j.Field("solveTolerance", v.solveTolerance);
    j.Field("maxAlignmentRms", v.maxAlignmentRms);
    j.Field("maxPairDistance", v.maxPairDistance);
    j.Field("minPairCoverage", v.minPairCoverage);
    j.Field("minPairNormalCosine", v.minPairNormalCosine);
    j.Field("huberDistance", v.huberDistance);
    j.Field("iterationLimit", v.iterationLimit);
    j.Field("requiresMeasurementQuality", v.requiresMeasurementQuality);
    j.Field("minSupportRatio", v.minSupportRatio);
    j.Field("maxLocalizationSigma", v.maxLocalizationSigma);
    j.Field("maxSurfaceFitResidual", v.maxSurfaceFitResidual);
    j.End();
  }
};
template <> struct Codec<AlignmentGeometrySupport> {
  static void Write(Json &j, const AlignmentGeometrySupport &v) {
    j.Object();
    j.Field("principalRmsSpreads", v.principalRmsSpreads);
    j.Field("axialSpan", v.axialSpan);
    j.Field("angularSpanDegrees", v.angularSpanDegrees);
    j.Field("radialDirectionMeanLength", v.radialDirectionMeanLength);
    j.End();
  }
};
template <> struct Codec<AlignmentGeometry> {
  static void Write(Json &j, const AlignmentGeometry &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("kind", v.kind);
    j.Field("sourceCenter", v.sourceCenter);
    j.Field("sourceDirection", v.sourceDirection);
    j.Field("radius", v.radius);
    j.Field("fitRms", v.fitRms);
    j.Field("fitMax", v.fitMax);
    j.Field("sampleCount", v.sampleCount);
    j.Field("rejectedCount", v.rejectedCount);
    j.Field("support", v.support);
    j.End();
  }
};
template <> struct Codec<AlignmentDiagnostics> {
  static void Write(Json &j, const AlignmentDiagnostics &v) {
    j.Object();
    j.Field("status", v.status);
    j.Field("hardRemaining", v.hardRemaining);
    j.Field("remaining", v.remaining);
    j.Field("singularValues", v.singularValues);
    j.Field("nullspace", v.nullspace);
    j.Field("stageRemaining", v.stageRemaining);
    j.Field("lengthScale", v.lengthScale);
    j.Field("rankTolerance", v.rankTolerance);
    j.Field("condition", v.condition);
    j.Field("rms", v.rms);
    j.Field("maximum", v.maximum);
    j.Field("iterations", v.iterations);
    j.Field("rejectedPairs", v.rejectedPairs);
    j.Field("isQualityPassed", v.isQualityPassed);
    j.Field("message", v.message);
    j.Field("prealignment", v.prealignment);
    j.Field("surfaceStages", v.surfaceStages);
    j.End();
  }
};
template <> struct Codec<AlignmentReferenceSnapshot> {
  static void Write(Json &j, const AlignmentReferenceSnapshot &v) {
    j.Object();
    j.Field("mesh", v.mesh);
    j.Field("filePath", v.filePath);
    j.Field("sha256", v.sha256);
    j.Field("frameId", v.frameId);
    j.Field("coordinateFrame", v.coordinateFrame);
    j.Field("fileUnit", v.fileUnit);
    j.Field("unit", v.unit);
    j.Field("sourceBytes", v.sourceBytes);
    j.Field("pointCount", v.pointCount);
    j.Field("triangleCount", v.triangleCount);
    j.End();
  }
};
template <> struct Codec<RecordColumn> {
  static void Write(Json &j, const RecordColumn &v) {
    j.Object();
    j.Field("name", v.name);
    j.Field("variantIndex", v.values.index());
    j.Field("values", v.values);
    j.End();
  }
};
template <> struct Codec<RecordTablePayload> {
  static void Write(Json &j, const RecordTablePayload &v) {
    j.Object();
    j.Field("type", v.GetDataType());
    j.Field("rowCount", v.GetRowCount());
    j.Field("schema", v.GetSchemaName());
    j.Field("columns", v.GetColumns());
    j.End();
  }
};
template <> struct Codec<MetrologyReportAlignment> {
  static void Write(Json &j, const MetrologyReportAlignment &v) {
    j.Object();
    j.Field("result", v.result);
    j.Field("recipeRevision", v.recipeRevision);
    j.Field("transform", v.transform);
    j.Field("input", v.input);
    j.Field("sourceToTarget", v.sourceToTarget);
    j.Field("algorithmVersion", v.algorithmVersion);
    j.Field("recipe", v.recipe);
    j.Field("geometries", v.geometries);
    j.Field("residuals", v.residuals);
    j.Field("diagnostics", v.diagnostics);
    j.Field("initialPoses", v.initialPoses);
    j.Field("reference", v.reference);
    j.Field("wasCurrentAtCapture", v.wasCurrentAtCapture);
    j.End();
  }
};
template <> struct Codec<InspectionIdealGeometry> {
  static void Write(Json &j, const InspectionIdealGeometry &v) {
    j.Object();
    j.Field("center", v.center);
    j.Field("direction", v.direction);
    j.Field("radius", v.radius);
    j.End();
  }
};
template <> struct Codec<InspectionSection> {
  static void Write(Json &j, const InspectionSection &v) {
    j.Object();
    j.Field("pinnedMesh", v.pinnedMesh);
    j.Field("planePoint", v.planePoint);
    j.Field("planeNormal", v.planeNormal);
    j.Field("triangles", v.triangles);
    j.Field("sampleSpacing", v.sampleSpacing);
    j.Field("planeTolerance", v.planeTolerance);
    j.Field("selectionPoint", v.selectionPoint);
    j.Field("maxSelectionDistance", v.maxSelectionDistance);
    j.End();
  }
};
template <> struct Codec<InspectionSectionSample> {
  static void Write(Json &j, const InspectionSectionSample &v) {
    j.Object();
    j.Field("triangle", v.triangle);
    j.Field("barycentric", v.barycentric);
    j.Field("planeCorrection", v.planeCorrection);
    j.End();
  }
};
template <> struct Codec<InspectionSectionTrace> {
  static void Write(Json &j, const InspectionSectionTrace &v) {
    j.Object();
    j.Field("sourcePlanePoint", v.sourcePlanePoint);
    j.Field("sourcePlaneNormal", v.sourcePlaneNormal);
    j.Field("components", v.components);
    j.Field("closedLoops", v.closedLoops);
    j.Field("selectedSegments", v.selectedSegments);
    j.Field("sourceTriangleCount", v.sourceTriangleCount);
    j.Field("perimeter", v.perimeter);
    j.Field("sampleSpacing", v.sampleSpacing);
    j.Field("maxPlaneCorrection", v.maxPlaneCorrection);
    j.Field("samples", v.samples);
    j.End();
  }
};
template <> struct Codec<InspectionGeometrySpec> {
  static void Write(Json &j, const InspectionGeometrySpec &v) {
    j.Object();
    j.Field("fit", v.fit);
    j.Field("source", v.source);
    j.Field("ideal", v.ideal);
    j.Field("definitionSource", v.definitionSource);
    j.Field("section", v.section);
    j.End();
  }
};
template <> struct Codec<InspectionOperand> {
  static void Write(Json &j, const InspectionOperand &v) {
    j.Object();
    j.Field("geometry", v.geometry);
    j.Field("sectionPlane", v.sectionPlane);
    j.End();
  }
};
template <> struct Codec<InspectionMeasure> {
  static void Write(Json &j, const InspectionMeasure &v) {
    j.Object();
    j.Field("metric", v.metric);
    j.Field("first", v.first);
    j.Field("second", v.second);
    j.Field("maxParallelAngleDegrees", v.maxParallelAngleDegrees);
    j.End();
  }
};
template <> struct Codec<InspectionItem> {
  static void Write(Json &j, const InspectionItem &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("name", v.name);
    j.Field("measured", v.measured);
    j.Field("nominal", v.nominal);
    j.Field("nominalMeasure", v.nominalMeasure);
    j.Field("nominalSource", v.nominalSource);
    j.Field("deviationLimits", v.deviationLimits);
    j.End();
  }
};
template <> struct Codec<InspectionPlan> {
  static void Write(Json &j, const InspectionPlan &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("alignmentResult", v.alignmentResult);
    j.Field("geometries", v.geometries);
    j.Field("items", v.items);
    j.Field("lengthScale", v.lengthScale);
    j.Field("rankTolerance", v.rankTolerance);
    j.Field("conditionLimit", v.conditionLimit);
    j.Field("solveTolerance", v.solveTolerance);
    j.Field("iterationLimit", v.iterationLimit);
    j.Field("requiresMeasurementQuality", v.requiresMeasurementQuality);
    j.Field("minSupportRatio", v.minSupportRatio);
    j.Field("maxLocalizationSigma", v.maxLocalizationSigma);
    j.Field("maxSurfaceFitResidual", v.maxSurfaceFitResidual);
    j.End();
  }
};
template <> struct Codec<InspectionGeometry> {
  static void Write(Json &j, const InspectionGeometry &v) {
    j.Object();
    j.Field("source", v.source);
    j.Field("status", v.status);
    j.Field("fitted", v.fitted);
    j.Field("alignedCenter", v.alignedCenter);
    j.Field("alignedDirection", v.alignedDirection);
    j.Field("section", v.section);
    j.Field("message", v.message);
    j.End();
  }
};
template <> struct Codec<InspectionItemResult> {
  static void Write(Json &j, const InspectionItemResult &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("measured", v.measured);
    j.Field("nominal", v.nominal);
    j.Field("deviation", v.deviation);
    j.Field("verdict", v.verdict);
    j.Field("message", v.message);
    j.End();
  }
};
template <> struct Codec<InspectionReport> {
  static void Write(Json &j, const InspectionReport &v) {
    j.Object();
    j.Field("geometries", v.geometries);
    j.Field("items", v.items);
    j.Field("verdict", v.verdict);
    j.Field("algorithmVersion", v.algorithmVersion);
    j.Field("evaluatedUnixMilliseconds", v.evaluatedUnixMilliseconds);
    j.End();
  }
};
template <> struct Codec<InspectionSnapshot> {
  static void Write(Json &j, const InspectionSnapshot &v) {
    j.Object();
    j.Field("result", v.result);
    j.Field("alignmentResult", v.alignmentResult);
    j.Field("sourceMesh", v.sourceMesh);
    j.Field("referenceData", v.referenceData);
    j.Field("transform", v.transform);
    j.Field("unit", v.unit);
    j.Field("sourceToTarget", v.sourceToTarget);
    j.Field("plan", v.plan);
    j.Field("report", v.report);
    j.Field("isCurrent", v.isCurrent);
    j.End();
  }
};
template <> struct Codec<AlignmentDeviationRequest> {
  static void Write(Json &j, const AlignmentDeviationRequest &v) {
    j.Object();
    j.Field("alignmentResult", v.alignmentResult);
    j.Field("metric", v.metric);
    j.Field("sourceTriangles", v.sourceTriangles);
    j.Field("referenceTriangles", v.referenceTriangles);
    j.Field("maxDistance", v.maxDistance);
    j.Field("minNormalCosine", v.minNormalCosine);
    j.Field("tolerance", v.tolerance);
    j.Field("reverseCoverage", v.reverseCoverage);
    j.Field("requiresMeasurementQuality", v.requiresMeasurementQuality);
    j.Field("minSupportRatio", v.minSupportRatio);
    j.Field("maxLocalizationSigma", v.maxLocalizationSigma);
    j.Field("maxSurfaceFitResidual", v.maxSurfaceFitResidual);
    j.End();
  }
};
template <> struct Codec<AlignmentDeviationStatistics> {
  static void Write(Json &j, const AlignmentDeviationStatistics &v) {
    j.Object();
    j.Field("counts", v.counts);
    j.Field("areas", v.areas);
    j.Field("selectedArea", v.selectedArea);
    j.Field("evaluatedArea", v.evaluatedArea);
    j.Field("outOfToleranceArea", v.outOfToleranceArea);
    j.Field("minimum",
            v.hasValues ? std::optional<double>{v.minimum} : std::nullopt);
    j.Field("maximum",
            v.hasValues ? std::optional<double>{v.maximum} : std::nullopt);
    j.Field("mean", v.hasValues ? std::optional<double>{v.mean} : std::nullopt);
    j.Field("rms", v.hasValues ? std::optional<double>{v.rms} : std::nullopt);
    j.Field("absoluteP95",
            v.hasValues ? std::optional<double>{v.absoluteP95} : std::nullopt);
    j.Field("hasValues", v.hasValues);
    j.End();
  }
};
template <> struct Codec<AlignmentDeviationSample> {
  static void Write(Json &j, const AlignmentDeviationSample &v) {
    j.Object();
    j.Field("state", v.state);
    j.Field("area", v.area);
    const bool hasQuery = v.state != AlignmentDeviationState::NotSelected &&
                          v.state != AlignmentDeviationState::InvalidSource;
    j.Field("value", hasQuery ? std::optional<double>{v.value} : std::nullopt);
    j.Field("distance",
            hasQuery ? std::optional<double>{v.distance} : std::nullopt);
    j.Field("closestTriangle",
            hasQuery ? std::optional<std::uint64_t>{v.closestTriangle}
                     : std::nullopt);
    j.End();
  }
};
template <> struct Codec<AlignmentDeviationField> {
  static void Write(Json &j, const AlignmentDeviationField &v) {
    j.Object();
    j.Field("statistics", v.statistics);
    j.Field("sampleCount", v.samples.size());
    j.Field("samplesIncluded", j.sink.options.includeDeviationSamples);
    j.Field("sampleIndexSpace", std::string("original-mesh-vertex"));
    if (j.sink.options.includeDeviationSamples)
      j.Field("samples", v.samples);
    j.End();
  }
};
template <> struct Codec<AlignmentDeviationSnapshot> {
  static void Write(Json &j, const AlignmentDeviationSnapshot &v) {
    j.Object();
    j.Field("result", v.result);
    j.Field("alignmentResult", v.alignmentResult);
    j.Field("sourceMesh", v.sourceMesh);
    j.Field("referenceMesh", v.referenceMesh);
    j.Field("transform", v.transform);
    j.Field("parameters", v.parameters);
    j.Field("unit", v.unit);
    j.Field("sourceToTarget", v.sourceToTarget);
    j.Field("forward", v.forward);
    j.Field("reverse", v.reverse);
    j.Field("verdict", v.verdict);
    j.Field("isCurrent", v.isCurrent);
    j.End();
  }
};
template <> struct Codec<VoxelVolumeRequest> {
  static void Write(Json &j, const VoxelVolumeRequest &v) {
    j.Object();
    j.Field("id", v.id);
    j.Field("source", v.source);
    j.Field("method", v.method);
    j.Field("extent", v.extent);
    j.Field("storedValueRange", v.storedValueRange);
    j.Field("labels", v.labels);
    j.Field("validityImage", v.validityImage);
    j.Field("requireEnclosedSelection", v.requireEnclosedSelection);
    j.End();
  }
};
template <> struct Codec<VoxelVolumeStatistics> {
  static void Write(Json &j, const VoxelVolumeStatistics &v) {
    j.Object();
    j.Field("evaluatedExtent", v.evaluatedExtent);
    j.Field("total", v.total);
    j.Field("selected", v.selected);
    j.Field("unselected", v.unselected);
    j.Field("masked", v.masked);
    j.Field("noData", v.noData);
    j.Field("nonFinite", v.nonFinite);
    j.Field("invalidLabels", v.invalidLabels);
    j.Field("cellVolumeMm3", v.cellVolumeMm3);
    j.Field("observedVolumeMm3", v.observedVolumeMm3);
    j.Field("volumeMm3", v.volumeMm3);
    j.Field("touchesRegionBoundary", v.touchesRegionBoundary);
    j.Field("validity", v.validity);
    j.End();
  }
};
template <> struct Codec<VoxelVolumeSnapshot> {
  static void Write(Json &j, const VoxelVolumeSnapshot &v) {
    j.Object();
    j.Field("result", v.result);
    j.Field("source", v.source);
    j.Field("parameters", v.parameters);
    j.Field("geometry", v.geometry);
    j.Field("statistics", v.statistics);
    j.Field("algorithmVersion", v.algorithmVersion);
    j.Field("isCurrent", v.isCurrent);
    j.Field("volumeUnit", std::string("mm^3"));
    j.Field("scope",
            std::string("selected full cells within evaluated index extent; "
                        "not continuous surface or whole-part volume"));
    j.End();
  }
};
template <> struct Codec<MetrologyReportImage> {
  static void Write(Json &j, const MetrologyReportImage &v) {
    j.Object();
    j.Field("geometry", v.geometry);
    j.Field("metadata", v.metadata);
    j.Field("valueType", v.valueType);
    j.Field("components", v.components);
    j.Field("valueBytes", v.valueBytes);
    j.Field("scalarRange", v.scalarRange);
    j.Field("hasValidityMask", v.hasValidityMask);
    j.End();
  }
};
template <> struct Codec<MetrologyReportMesh> {
  static void Write(Json &j, const MetrologyReportMesh &v) {
    j.Object();
    j.Field("vertices", v.vertices);
    j.Field("triangles", v.triangles);
    j.Field("coordinateFrame", v.coordinateFrame);
    j.End();
  }
};
template <> struct Codec<MetrologyReportInput> {
  static void Write(Json &j, const MetrologyReportInput &v) {
    j.Object();
    j.Field("revision", v.revision);
    j.Field("type", v.type);
    j.Field("inputs", v.inputs);
    j.Field("provenance", v.provenance);
    j.Field("image", v.image);
    j.Field("mesh", v.mesh);
    j.Field("wasCurrentAtCapture", v.wasCurrentAtCapture);
    j.End();
  }
};
template <> struct Codec<MetrologyReport> {
  static void Write(Json &j, const MetrologyReport &v) {
    j.Object();
    j.Field("format", std::string("mvvcvtk.metrology.report.json/2"));
    j.Field("schemaVersion", v.schemaVersion);
    j.Field("id", v.id);
    j.Field("title", v.title);
    j.Field("capturedCommit", v.capturedCommit);
    j.Field("capturedUnixMilliseconds", v.capturedUnixMilliseconds);
    j.Field("alignments", v.alignments);
    j.Field("inspections", v.inspections);
    j.Field("deviations", v.deviations);
    j.Field("voxelVolumes", v.voxelVolumes);
    j.Field("inputs", v.inputs);
    j.Field("wereInputsCurrentAtCapture", v.wereInputsCurrentAtCapture);
    j.Field("estimatedMetadataBytes", v.estimatedMetadataBytes);
    j.End();
  }
};

const MetrologyReportAlignment &Owner(const MetrologyReport &r,
                                      const DataRevisionRef &ref) {
  const auto found = std::lower_bound(
      r.alignments.begin(), r.alignments.end(), ref,
      [](const auto &a, const auto &b) { return a.result < b; });
  Require(found != r.alignments.end() && found->result == ref,
          "Report evaluation has no alignment owner.");
  return *found;
}
void Validate(const MetrologyReport &r, Sink &sink) {
  Require(r.schemaVersion == 2 && !r.id.empty() &&
              r.capturedUnixMilliseconds > 0 &&
              (!r.alignments.empty() || !r.voxelVolumes.empty()),
          "Invalid frozen report header.");
  for (std::size_t i = 0; i < r.alignments.size(); ++i) {
    sink.Check();
    const auto &a = r.alignments[i];
    Require(GetDataRevisionRefValid(a.result) &&
                (!i || r.alignments[i - 1].result < a.result) && a.recipe &&
                a.geometries && a.residuals && a.diagnostics && a.initialPoses,
            "Invalid or unsorted report alignment context.");
    Require(a.residuals->GetValid(), "Invalid report residual table.");
    Require(std::all_of(a.sourceToTarget.begin(), a.sourceToTarget.end(),
                        [](double v) { return std::isfinite(v); }),
            "Non-finite report transform.");
  }
  for (const auto &v : r.voxelVolumes) {
    sink.Check();
    Require(AlignmentVoxelVolumeData::GetSnapshotValid(v),
            "Invalid voxel volume report snapshot.");
    const auto input =
        std::find_if(r.inputs.begin(), r.inputs.end(),
                     [&](const auto &i) { return i.revision == v.source; });
    Require(input != r.inputs.end(), "Voxel volume source lineage is missing.");
    if (v.parameters->method == VoxelVolumeMethod::StoredValueRange)
      Require(input->image && AlignmentVoxelVolume::GetGeometrySame(
                                  input->image->geometry, v.geometry),
              "Voxel source geometry mismatch.");
    else
      Require(input->type == DataTypes::labelMap3D,
              "Voxel label source type mismatch.");
    if (v.parameters->validityImage) {
      const auto validity =
          std::find_if(r.inputs.begin(), r.inputs.end(), [&](const auto &i) {
            return i.revision == *v.parameters->validityImage;
          });
      Require(validity != r.inputs.end() && validity->image &&
                  AlignmentVoxelVolume::GetGeometrySame(
                      validity->image->geometry, v.geometry),
              "Voxel validity image lineage mismatch.");
    }
  }
  for (const auto &v : r.inspections) {
    sink.Check();
    const auto &a = Owner(r, v.alignmentResult);
    Require(a.diagnostics->status == AlignmentStatus::FullyDetermined &&
                a.diagnostics->isQualityPassed && v.plan && v.report &&
                v.plan->alignmentResult == v.alignmentResult &&
                v.transform == a.transform && v.sourceMesh == a.input.mesh &&
                v.referenceData == a.recipe->nominalData &&
                v.unit == a.input.unit &&
                v.sourceToTarget == a.sourceToTarget &&
                v.report->items.size() == v.plan->items.size() &&
                v.report->geometries.size() == v.plan->geometries.size(),
            "Inspection report context mismatch.");
    for (std::size_t i = 0; i < v.report->items.size(); ++i)
      Require(v.report->items[i].id == v.plan->items[i].id,
              "Inspection item identity mismatch.");
  }
  for (const auto &v : r.deviations) {
    sink.Check();
    const auto &a = Owner(r, v.alignmentResult);
    Require(a.diagnostics->status == AlignmentStatus::FullyDetermined &&
                a.diagnostics->isQualityPassed && v.parameters && v.forward &&
                v.parameters->alignmentResult == v.alignmentResult &&
                v.transform == a.transform && v.sourceMesh == a.input.mesh &&
                v.referenceMesh == a.recipe->nominalData &&
                v.unit == a.input.unit && v.sourceToTarget == a.sourceToTarget,
            "Deviation report context mismatch.");
  }
}
const char *Unit(AlignmentUnit unit) {
  (void)Name(unit);
  return unit == AlignmentUnit::Millimeter ? "mm"
         : unit == AlignmentUnit::Meter    ? "m"
                                           : "model-unit";
}
class Csv final {
public:
  explicit Csv(Sink &sink) : sink(sink) {}
  void Cell(std::string_view text) {
    Begin();
    sink.Put("\"");
    if (!text.empty() && std::string_view("=+-@' \t\r\n").find(text.front()) !=
                             std::string_view::npos)
      sink.Put("'");
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size();) {
      if (i - start >= 4096) {
        sink.Put(text.substr(start, i - start));
        start = i;
      }
      sink.Check();
      Require(static_cast<unsigned char>(text[i]) >= 32 || text[i] == '\t' ||
                  text[i] == '\r' || text[i] == '\n',
              "Unsupported CSV control character; use JSON for exact text.");
      if (text[i] == '"') {
        sink.Put(text.substr(start, i - start));
        sink.Put("\"\"");
        ++i;
        start = i;
      } else
        i += Utf8(text, i);
    }
    sink.Put(text.substr(start));
    sink.Put("\"");
  }
  void Empty() { Begin(); }
  void Numeric(std::optional<double> value) {
    Begin();
    if (!value)
      return;
    if (std::isfinite(*value))
      Number(sink, *value);
    else
      sink.Put(std::isnan(*value) ? "NaN"
               : *value > 0       ? "PositiveInfinity"
                                  : "NegativeInfinity");
  }
  template <class T> void Integer(T value) {
    Begin();
    Number(sink, value);
  }
  void Boolean(bool value) {
    Begin();
    sink.Put(value ? "true" : "false");
  }
  void Matrix(const AlignmentMatrix &value) {
    Begin();
    sink.Put("\"[");
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i)
        sink.Put(",");
      Number(sink, value[i]);
    }
    sink.Put("]\"");
  }
  template <class Range> void Array(const Range &value) {
    Begin();
    sink.Put("\"[");
    bool separator = false;
    for (const auto item : value) {
      if (separator)
        sink.Put(",");
      Number(sink, item);
      separator = true;
    }
    sink.Put("]\"");
  }
  void End() {
    while (columns < 46)
      Empty();
    Require(columns == 46, "CSV column count mismatch.");
    sink.Put("\r\n");
    first = true;
    columns = 0;
  }

private:
  void Begin() {
    ++columns;
    if (!first)
      sink.Put(",");
    first = false;
  }
  Sink &sink;
  bool first = true;
  unsigned columns = 0;
};
void CsvReport(Sink &sink, const MetrologyReport &r) {
  Csv csv(sink);
  for (const auto *name : {"record_type",
                           "report_id",
                           "captured_commit",
                           "captured_unix_ms",
                           "all_inputs_current_at_capture",
                           "result",
                           "alignment",
                           "transform",
                           "source_mesh",
                           "reference",
                           "source_frame",
                           "target_frame",
                           "coordinate_frame",
                           "model_unit",
                           "source_to_target",
                           "alignment_quality_passed",
                           "item_id",
                           "name",
                           "metric",
                           "value_unit",
                           "measured",
                           "nominal",
                           "deviation",
                           "lower_limit",
                           "upper_limit",
                           "verdict",
                           "status",
                           "current_at_capture",
                           "message",
                           "vertex",
                           "closest_triangle",
                           "distance",
                           "area",
                           "sample_state",
                           "source_volume",
                           "validity_image",
                           "voxel_method",
                           "voxel_extent",
                           "voxel_range",
                           "voxel_labels",
                           "voxel_spacing",
                           "voxel_origin",
                           "voxel_direction",
                           "require_enclosed_selection",
                           "touches_region_boundary",
                           "algorithm_version"})
    csv.Cell(name);
  csv.End();
  const auto prefix = [&](const char *kind, const DataRevisionRef &result,
                          const MetrologyReportAlignment &a) {
    csv.Cell(kind);
    csv.Cell(r.id);
    csv.Integer(r.capturedCommit);
    csv.Integer(r.capturedUnixMilliseconds);
    csv.Boolean(r.wereInputsCurrentAtCapture);
    csv.Cell(Ref(result));
    csv.Cell(Ref(a.result));
    csv.Cell(Ref(a.transform));
    csv.Cell(Ref(a.input.mesh));
    csv.Cell(Ref(a.recipe->nominalData));
    csv.Cell(a.input.sourceFrameId);
    csv.Cell(a.recipe->targetFrameId);
    csv.Cell(a.input.coordinateFrame);
    csv.Cell(Unit(a.input.unit));
    csv.Matrix(a.sourceToTarget);
    csv.Boolean(a.diagnostics->isQualityPassed);
  };
  const auto emptySample = [&] {
    for (int i = 0; i < 5; ++i)
      csv.Empty();
    csv.End();
  };
  for (const auto &v : r.voxelVolumes) {
    const auto &p = *v.parameters;
    const auto &s = v.statistics;
    const auto row = [&](const char *metric, const char *unit,
                         std::optional<double> value,
                         std::optional<std::uint64_t> count = {}) {
      csv.Cell("voxel-volume");
      csv.Cell(r.id);
      csv.Integer(r.capturedCommit);
      csv.Integer(r.capturedUnixMilliseconds);
      csv.Boolean(r.wereInputsCurrentAtCapture);
      csv.Cell(Ref(v.result));
      for (int i = 0; i < 6; ++i)
        csv.Empty();
      csv.Cell(v.geometry.coordinateFrame);
      csv.Cell("mm");
      csv.Empty();
      csv.Empty();
      csv.Cell(p.id);
      csv.Cell("selected cells in index ROI");
      csv.Cell(metric);
      csv.Cell(unit);
      if (count)
        csv.Integer(*count);
      else
        csv.Numeric(value);
      for (int i = 0; i < 5; ++i)
        csv.Empty();
      csv.Cell(Name(s.validity));
      csv.Boolean(v.isCurrent);
      csv.Cell("Full-cell ROI estimate; not continuous surface or whole-part "
               "volume");
      for (int i = 0; i < 5; ++i)
        csv.Empty();
      csv.Cell(Ref(v.source));
      if (p.validityImage)
        csv.Cell(Ref(*p.validityImage));
      else
        csv.Empty();
      csv.Cell(Name(p.method));
      csv.Array(s.evaluatedExtent);
      if (p.storedValueRange)
        csv.Array(*p.storedValueRange);
      else
        csv.Empty();
      csv.Array(p.labels);
      csv.Array(v.geometry.spacing);
      csv.Array(v.geometry.origin);
      csv.Array(v.geometry.direction);
      csv.Boolean(p.requireEnclosedSelection);
      csv.Boolean(s.touchesRegionBoundary);
      csv.Cell(v.algorithmVersion);
      csv.End();
    };
    row("volume", "mm^3", s.volumeMm3);
    row("observed-volume", "mm^3", s.observedVolumeMm3);
    row("cell-volume", "mm^3", s.cellVolumeMm3);
    row("total", "count", {}, s.total);
    row("selected", "count", {}, s.selected);
    row("unselected", "count", {}, s.unselected);
    row("masked", "count", {}, s.masked);
    row("no-data", "count", {}, s.noData);
    row("non-finite", "count", {}, s.nonFinite);
    row("invalid-labels", "count", {}, s.invalidLabels);
  }
  for (const auto &a : r.alignments) {
    prefix("alignment", a.result, a);
    csv.Empty();
    csv.Cell(a.recipe->id);
    csv.Cell("alignment-rms");
    csv.Cell(Unit(a.input.unit));
    csv.Numeric(a.diagnostics->rms);
    for (int i = 0; i < 5; ++i)
      csv.Empty();
    csv.Cell(Name(a.diagnostics->status));
    csv.Boolean(a.wasCurrentAtCapture);
    csv.Cell(a.diagnostics->message);
    emptySample();
  }
  for (const auto &v : r.inspections) {
    const auto &a = Owner(r, v.alignmentResult);
    prefix("inspection-summary", v.result, a);
    csv.Empty();
    csv.Cell(v.plan->id);
    csv.Cell("inspection-verdict");
    csv.Empty();
    for (int i = 0; i < 5; ++i)
      csv.Empty();
    csv.Cell(Name(v.report->verdict));
    csv.Empty();
    csv.Boolean(v.isCurrent);
    csv.Empty();
    emptySample();
    for (std::size_t i = 0; i < v.report->items.size(); ++i) {
      const auto &row = v.report->items[i];
      const auto &item = v.plan->items[i];
      prefix("inspection", v.result, a);
      csv.Cell(row.id);
      csv.Cell(item.name);
      csv.Cell(Name(item.measured.metric));
      csv.Cell(item.measured.metric ==
                       InspectionMetric::OrientedDirectionAngleDegrees
                   ? "deg"
                   : Unit(v.unit));
      csv.Numeric(row.measured);
      csv.Numeric(row.nominal);
      csv.Numeric(row.deviation);
      csv.Numeric(item.deviationLimits
                      ? std::optional<double>{(*item.deviationLimits)[0]}
                      : std::nullopt);
      csv.Numeric(item.deviationLimits
                      ? std::optional<double>{(*item.deviationLimits)[1]}
                      : std::nullopt);
      csv.Cell(Name(row.verdict));
      csv.Empty();
      csv.Boolean(v.isCurrent);
      csv.Cell(row.message);
      emptySample();
    }
  }
  for (const auto &v : r.deviations) {
    const auto &a = Owner(r, v.alignmentResult);
    prefix("deviation-summary", v.result, a);
    csv.Empty();
    csv.Cell(Name(v.parameters->metric));
    csv.Cell("deviation-verdict");
    csv.Empty();
    for (int i = 0; i < 5; ++i)
      csv.Empty();
    csv.Cell(Name(v.verdict));
    csv.Empty();
    csv.Boolean(v.isCurrent);
    csv.Empty();
    emptySample();
    for (int direction = 0; direction < 2; ++direction) {
      const auto &field = direction ? v.reverse : v.forward;
      if (!field)
        continue;
      const auto &s = field->statistics;
      const auto *kind = direction ? "deviation-reverse" : "deviation-forward";
      const auto *metric =
          direction ? "UnsignedDistance" : Name(v.parameters->metric);
      const auto verdict =
          direction ? AlignmentInspectionVerdict::NotEvaluated : v.verdict;
      const auto statistics = [&](const char *name, const auto &value,
                                  const std::string &unit) {
        prefix(kind, v.result, a);
        csv.Empty();
        csv.Cell(metric);
        csv.Cell(name);
        csv.Cell(unit);
        if constexpr (std::is_integral_v<std::decay_t<decltype(value)>>)
          csv.Integer(value);
        else
          csv.Numeric(value);
        for (int i = 0; i < 4; ++i)
          csv.Empty();
        csv.Cell(Name(verdict));
        csv.Empty();
        csv.Boolean(v.isCurrent);
        csv.Empty();
        emptySample();
      };
      for (const auto &quantity :
           std::array<std::pair<const char *, double>, 5>{
               {{"minimum", s.minimum},
                {"maximum", s.maximum},
                {"mean", s.mean},
                {"rms", s.rms},
                {"absolute-p95", s.absoluteP95}}})
        statistics(quantity.first,
                   s.hasValues ? std::optional<double>{quantity.second}
                               : std::nullopt,
                   Unit(v.unit));
      const std::string areaUnit = std::string(Unit(v.unit)) + "^2";
      statistics("selected-area", s.selectedArea, areaUnit);
      statistics("evaluated-area", s.evaluatedArea, areaUnit);
      statistics("out-of-tolerance-area",
                 !direction && v.parameters->tolerance
                     ? std::optional<double>{s.outOfToleranceArea}
                     : std::nullopt,
                 areaUnit);
      for (std::size_t state = 0; state < s.counts.size(); ++state) {
        const std::string label =
            Name(static_cast<AlignmentDeviationState>(state));
        statistics((label + "-count").c_str(), s.counts[state], "count");
        statistics((label + "-area").c_str(), s.areas[state], areaUnit);
      }
      if (!direction && v.parameters->tolerance) {
        prefix("deviation-tolerance", v.result, a);
        csv.Empty();
        csv.Cell(metric);
        csv.Cell("per-sample-limits");
        csv.Cell(Unit(v.unit));
        for (int i = 0; i < 3; ++i)
          csv.Empty();
        csv.Numeric((*v.parameters->tolerance)[0]);
        csv.Numeric((*v.parameters->tolerance)[1]);
        csv.Cell(Name(v.verdict));
        csv.Empty();
        csv.Boolean(v.isCurrent);
        csv.Empty();
        emptySample();
      }
      if (sink.options.includeDeviationSamples)
        for (std::size_t vertex = 0; vertex < field->samples.size(); ++vertex) {
          const auto &sample = field->samples[vertex];
          const bool valid = sample.state == AlignmentDeviationState::Valid;
          prefix(direction ? "deviation-reverse-sample"
                           : "deviation-forward-sample",
                 v.result, a);
          csv.Empty();
          csv.Cell(metric);
          csv.Cell("sample");
          csv.Cell(Unit(v.unit));
          csv.Numeric(valid ? std::optional<double>{sample.value}
                            : std::nullopt);
          for (int i = 0; i < 4; ++i)
            csv.Empty();
          csv.Cell(Name(verdict));
          csv.Empty();
          csv.Boolean(v.isCurrent);
          csv.Empty();
          csv.Integer(vertex);
          if (valid)
            csv.Integer(sample.closestTriangle);
          else
            csv.Empty();
          csv.Numeric(valid ? std::optional<double>{sample.distance}
                            : std::nullopt);
          csv.Numeric(sample.area);
          csv.Cell(Name(sample.state));
          csv.End();
        }
    }
  }
}
} // namespace

MetrologyReportWriteResult
WriteMetrologyReport(const MetrologyReport &report, std::ostream &output,
                     const MetrologyReportWriteOptions &options) {
  Sink sink(output, options);
  MetrologyReportWriteResult result;
  try {
    Require(static_cast<unsigned>(options.format) <= 1,
            "Unknown report output format.");
    sink.Check();
    Validate(report, sink);
    if (options.format == MetrologyReportFormat::Json) {
      Json json(sink);
      json.Value(report);
      sink.Put("\n");
    } else
      CsvReport(sink, report);
    sink.Flush();
    result.status = Status::Succeeded;
  } catch (const Failure &e) {
    result.status = e.status;
    result.message = e.message;
  } catch (const std::ios_base::failure &e) {
    result.status = Status::IoError;
    result.message = e.what();
  } catch (const std::bad_alloc &) {
    result.status = Status::BudgetExceeded;
  } catch (const std::exception &e) {
    result.status = Status::InternalError;
    result.message = e.what();
  } catch (...) {
    result.status = Status::InternalError;
    result.message = "Report writer failed.";
  }
  result.bytesWritten = sink.confirmed;
  return result;
}
