// 测试用途：从普通 Host API 验证接纳/完成边界、历史值查询和快照独立性。
#include "QtHostMethodCases.h"
#include "Host/VtkAppHostSession.h"
#include <vtkRenderWindow.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>

int GetConsumerContractFailCount()
{
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        failures += !GetCaseResult(value, message);
    };
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    HostRenderViewConfig view;
    view.id = "consumer-contract";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    view.renderWindow->SetOffScreenRendering(1);
    config.renderViews.push_back(view);
    auto ownedSession = std::make_shared<VtkAppHostSession>(std::move(config));
    auto& session = *ownedSession;
    check(!session.GetDataCatalog(), "catalog does not build an unready Session");
    if (!session.BuildSession()) return failures + 1;
    const auto owner = std::this_thread::get_id();
    QJsonObject real;
    const auto manifestPath = qgetenv("MVVCVTK_HOST_CONSUMER_REAL_MANIFEST");
    if (!manifestPath.isEmpty()) {
        QFile file(QString::fromUtf8(manifestPath));
        if (!file.open(QIODevice::ReadOnly)) return failures + 1;
        real = QJsonDocument::fromJson(file.readAll()).object();
        QFile input(real["filePath"].toString());
        if (real["evidenceKind"].toString() != "real-data-roi" || real["inputFormat"].toString() != "float32-le-xfastest"
            || !input.open(QIODevice::ReadOnly)
            || QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()
                != real["sha256"].toString().toLatin1()) return failures + 1;
    }
    const auto load = [&](const char* id, float value) {
        std::unique_ptr<HostRequest> request;
        if (real.isEmpty()) {
            auto reload = std::make_unique<HostReloadRequest>();
            reload->geometry.dimensions = {2, 2, 2};
            reload->geometry.spacing = {.7F, 1.1F, 1.3F};
            reload->geometry.origin = {12.F, -17.F, 23.F};
            reload->metadata.identity.datasetId = id;
            reload->metadata.source.kind = ImageSourceKind::Memory;
            reload->metadata.source.uri = std::string("memory://") + id;
            reload->voxels.assign(8, value);
            request = std::move(reload);
        } else {
            auto raw = std::make_unique<HostLoadRequest>();
            raw->filePath = real["filePath"].toString().toStdString();
            for (int axis = 0; axis < 3; ++axis) {
                raw->geometry.dimensions[axis] = real["dimensions"].toArray()[axis].toInt();
                raw->geometry.spacing[axis] = static_cast<float>(real["spacingLPS"].toArray()[axis].toDouble());
                raw->geometry.origin[axis] = static_cast<float>(real["originLPS"].toArray()[axis].toDouble());
            }
            for (int index = 0; index < 9; ++index)
                raw->geometry.direction[index] = real["directionLPS"].toArray()[index].toDouble();
            raw->metadata.identity.datasetId = id;
            raw->metadata.source.kind = ImageSourceKind::RawFile;
            raw->metadata.source.uri = raw->filePath;
            raw->metadata.source.digest = real["sha256"].toString().toStdString();
            request = std::move(raw);
        }
        struct CompleteState final {
            std::mutex mutex;
            int count = 0;
            HostResult result;
            bool isOwner = false;
            bool queryReady = false;
        };
        auto state = std::make_shared<CompleteState>();
        const std::weak_ptr<VtkAppHostSession> weak = ownedSession;
        const auto admission = session.SendRequest(std::move(*request), [state, weak, owner](HostResult completed) {
            const auto host = weak.lock();
            const bool ready = host && host->GetDataCatalog().has_value();
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->isOwner = std::this_thread::get_id() == owner;
            state->queryReady = ready;
            state->result = std::move(completed);
            ++state->count;
        });
        if (admission.status != HostAdmissionStatus::Accepted) throw std::runtime_error(admission.message);
        const auto count = [&] { const std::lock_guard<std::mutex> lock(state->mutex); return state->count; };
        // 仅为测试等待设置安全 deadline，不作为产品时延指标。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!count() && std::chrono::steady_clock::now() < deadline) {
            session.SendUpdates();
            std::this_thread::yield();
        }
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            if (state->count != 1 || !state->result.isSucceeded) throw std::runtime_error("load completion failed");
            check(state->isOwner, "completion runs on owner thread");
            check(state->queryReady, "completion can reenter a value query");
        }
        session.SendUpdates();
        check(count() == 1, "accepted completion is delivered exactly once");
        return *session.GetImageDescriptor();
    };
    try {
        const auto first = load("consumer-first", 7.F);
        const auto frozen = session.GetDataCatalog();
        check(frozen && std::any_of(frozen->data.begin(), frozen->data.end(), [&](const auto& item) {
            return item.revision == first.dataRevision && item.type == DataTypes::imageGrid3D;
        }), "catalog includes the exact published image revision");
        DataQuery query;
        query.entityId = first.dataRevision.entityId;
        query.type = DataTypes::imageGrid3D;
        const auto filtered = session.GetDataCatalog(query);
        check(filtered && filtered->data.size() == 1 && filtered->data.front().revision == first.dataRevision,
            "catalog applies entity and type filters");
        const auto historical = session.GetImageDescriptor(first.dataRevision);
        check(historical && historical->dims == first.dims && historical->spacing == first.spacing
            && historical->origin == first.origin && historical->scalarRange == first.scalarRange
            && historical->bindingRevision == first.bindingRevision
            && historical->componentBytes == sizeof(float) && historical->componentCount == 1
            && historical->componentBytes == first.componentBytes && historical->byteOrder == first.byteOrder
            && historical->tupleOrder == first.tupleOrder, "exact image query preserves geometry, layout and current binding");
        if (!real.isEmpty()) check(first.metadata.source.digest == real["sha256"].toString().toStdString()
            && first.metadata.source.kind == ImageSourceKind::RawFile,
            "real input keeps its verified source identity through the public loader");
        const auto second = load("consumer-second", 42.F);
        const auto old = session.GetImageDescriptor(first.dataRevision);
        check(second.dataRevision != first.dataRevision && old && old->dataRevision == first.dataRevision
            && old->scalarRange == first.scalarRange && old->bindingRevision == 0,
            "historical query does not silently select the new primary image");
        const auto now = session.GetDataCatalog();
        check(frozen && now && frozen->commitId != now->commitId && frozen->data.size() < now->data.size(),
            "catalog snapshots remain independent after publication");
        check(!session.GetImageDescriptor(DataRevisionRef{{"unknown-image"}, 1}), "unknown revision has no image descriptor");
        HostAdmission wrong;
        int rejectedCompletions = 0;
        bool wrongQueriesRejected = false;
        std::thread thread([&] {
            HostViewSetRequest request;
            request.targetView.viewId = view.id;
            request.opacity = .5;
            wrong = session.SendRequest(std::move(request), [&](HostResult) { ++rejectedCompletions; });
            wrongQueriesRejected = !session.GetDataCatalog() && !session.GetImageDescriptor(first.dataRevision);
        });
        thread.join();
        check(wrong.status == HostAdmissionStatus::Rejected && wrong.errorCode == HostErrorCode::WrongThread
            && rejectedCompletions == 0 && wrongQueriesRejected, "wrong-thread rejection never calls user completion or reads payload");
        HostReloadRequest invalid;
        invalid.geometry.dimensions = {2, 2, 2};
        invalid.voxels.assign(7, 1.F);
        const auto rejected = session.SendRequest(std::move(invalid), [&](HostResult) { ++rejectedCompletions; });
        session.SendUpdates();
        check(rejected.status == HostAdmissionStatus::Rejected && rejected.errorCode == HostErrorCode::RequestRejected
            && rejectedCompletions == 0, "invalid request has admission diagnostics and no later completion");
        check(session.Stop(), "consumer Session stops");
        check(!session.GetDataCatalog() && !session.GetImageDescriptor(first.dataRevision), "stopped Session rejects historical queries");
        check(frozen && std::any_of(frozen->data.begin(), frozen->data.end(), [&](const auto& item) {
            return item.revision == first.dataRevision;
        }),
            "value catalog survives Session Stop");
    }
    catch (const std::exception& error) {
        GetCaseResult(false, error.what());
        ++failures;
        (void)session.Stop();
    }
    return failures;
}
