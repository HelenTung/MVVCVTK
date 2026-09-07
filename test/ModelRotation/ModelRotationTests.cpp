#include "App/AppState.h"
#include "Algorithms/ModelRotationAlgorithm.h"
#include "Host/ModelRotationHostFeature.h"
#include "Host/HostFrameCoordinator.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

using Math = ModelRotationAlgorithm;
constexpr Math::Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

void TestMath()
{
    auto start = identity;
    start[0] = 2; start[5] = 3; start[10] = 4;
    start[3] = 10; start[7] = 20; start[11] = 30;
    const auto rotation = Math::GetAxisRotation({0,0,1}, 90);
    CHECK(rotation);
    const auto matrix = Math::GetRotatedMatrix(start, {10,20,30}, *rotation);
    CHECK(matrix);
    const auto origin = Math::GetWorldPoint(*matrix, {0,0,0});
    const auto point = Math::GetWorldPoint(*matrix, {1,0,0});
    CHECK(std::abs(origin[0]-10) < 1e-10 && std::abs(origin[1]-20) < 1e-10);
    CHECK(std::abs(point[0]-10) < 1e-10 && std::abs(point[1]-22) < 1e-10);
    CHECK(!Math::GetAxisRotation({0,0,0}, 30));
    CHECK(!Math::GetAxisRotation({0,0,1}, std::numeric_limits<double>::infinity()));
    ImageDescriptor image;
    image.extent = {-2,4, 2,6, -3,1};
    image.spacing = {2,3,4};
    image.origin = {10,20,30};
    image.direction = {0,-1,0, 1,0,0, 0,0,1};
    const auto center = Math::GetModelCenter(image);
    CHECK(center && (*center == Math::Point{-2,22,26}));
    const auto q = Math::GetTrackballRotation(-100,0,100,0,100,
        {1,0,0},{0,1,0},{0,0,1});
    CHECK(q && Math::GetRotatedMatrix(identity,{0,0,0},*q));
}

DataReadyState BuildReady(std::uint64_t revision = 1)
{
    DataReadyState ready;
    ready.dataRevision.entityId.bytes[0] = 1;
    ready.dataRevision.generation = revision;
    ready.bindingRevision = revision;
    return ready;
}

void TestState()
{
    SharedInteractionState state;
    state.SetDataReady(BuildReady());
    state.SetTransformGeneration(17);
    auto before = state.GetTransformState();
    CHECK(state.SetInteracting({"Crop","drag"},true));
    CHECK(!state.StartTransform("model-rotation",before));
    CHECK(state.SetInteracting({"Crop","drag"},false));
    const auto token = state.StartTransform("model-rotation",before);
    CHECK(token);
    CHECK(!state.SetInteracting({"Crop","drag"},true));
    CHECK(!state.SetModelMatrix(identity));
    auto candidate = identity; candidate[3] = 9;
    for (std::uint64_t i=1; i<=100000; ++i)
        CHECK(state.SetTransformPreview("model-rotation",*token,i,candidate));
    CHECK(state.GetTransformState().hasPending);
    CHECK(state.GetTransformState().modelToWorld == before.modelToWorld);
    CHECK(!state.SetTransformPreview("model-rotation",*token,1,identity));
    CHECK(!state.SetTransformPreview("Other",*token,100001,identity));
    auto invalid = candidate; invalid[5] = 0;
    CHECK(!state.SetTransformPreview("model-rotation",*token,100001,invalid));
    CHECK(state.StartTransformFrame());
    CHECK(state.GetModelMatrix() == candidate);
    CHECK(state.GetTransformState().modelToWorld == before.modelToWorld);
    CHECK(!state.StopTransform("model-rotation",*token));
    CHECK(state.ClearTransformFrame());
    CHECK(state.GetModelMatrix() == before.modelToWorld);
    CHECK(state.GetTransformState().hasPending);
    CHECK(state.StartTransformFrame());
    state.SetTransformFrameCommit();
    CHECK(state.GetTransformState().modelToWorld == candidate);
    CHECK(state.GetTransformState().dataRevision == before.dataRevision);
    CHECK(state.GetTransformState().bindingRevision == before.bindingRevision);
    CHECK(state.StopTransform("model-rotation",*token));
    CHECK(!state.SetTransformPreview("model-rotation",*token,100002,identity));
    CHECK(state.StartTransformFrame());
    state.SetTransformFrameCommit();
    CHECK(state.GetTransformState().modelToWorld == before.modelToWorld);
    CHECK(state.GetTransformState().completion == ModelTransformStatus::Cancelled);
    CHECK(!state.StartTransform("model-rotation",before));
    before = state.GetTransformState();
    const auto second = state.StartTransform("model-rotation",before);
    CHECK(second);
    candidate[3] = 25;
    CHECK(state.SetTransformPreview("model-rotation",*second,1,candidate,true));
    CHECK(state.StartTransformFrame());
    state.SetTransformFrameCommit();
    CHECK(state.GetModelMatrix() == candidate);
    CHECK(!state.StopTransform("model-rotation",*second));
    const auto third = state.StartTransform("model-rotation",state.GetTransformState());
    CHECK(third);
    candidate[3] = 50;
    CHECK(state.SetTransformPreview("model-rotation",*third,1,candidate));
    CHECK(state.StartTransformFrame());
    state.SetDataReady(BuildReady(2));
    CHECK(!state.GetTransformFrameValid());
    CHECK(!state.SetTransformFrameCommit());
    CHECK(state.ClearTransformFrame());
    CHECK(state.GetModelMatrix()[3] == 25);
    CHECK(!state.StopTransform("model-rotation",*third));
    CHECK(state.GetTransformState().completion == ModelTransformStatus::Invalidated);
    const auto selected = state.StartTransform("model-rotation", state.GetTransformState());
    CHECK(selected && state.SetTransformPreview("model-rotation", *selected, 1, candidate));
    CHECK(state.StartTransformFrame());
    state.SetImageDataReady(BuildReady(3));
    CHECK(!state.GetTransformFrameValid() && !state.SetTransformFrameCommit());
    CHECK(state.ClearTransformFrame() && state.GetModelMatrix()[3] == 25);
    CHECK(!state.StopTransform("model-rotation", *selected));
    CHECK(state.GetTransformState().completion == ModelTransformStatus::Invalidated);
    bool wrongThreadAccepted = true;
    const auto expected = state.GetTransformState();
    std::thread wrongThread([&] {
        wrongThreadAccepted = state.StartTransform("model-rotation",expected).has_value();
    });
    wrongThread.join();
    CHECK(!wrongThreadAccepted);
    state.SetTransformGeneration(18);
    CHECK(!state.StartTransform("model-rotation",expected));
}

void TestFrameFailure()
{
    SharedInteractionState state;
    state.SetDataReady(BuildReady());
    state.SetTransformGeneration(1);
    const auto token = state.StartTransform("model-rotation",state.GetTransformState());
    auto matrix = identity; matrix[3] = 7;
    CHECK(token && state.SetTransformPreview("model-rotation",*token,1,matrix,true));
    bool failsStage = true, failsRender = true, hasRender = false;
    HostFrameCoordinator::Callbacks callbacks;
    callbacks.collectUpdates = [] { return true; };
    callbacks.sendFeatureTicks = [] {};
    callbacks.setIntents = [](const auto&) { return true; };
    callbacks.applyFeatureUpdates = [&] { return state.StartTransformFrame(); };
    callbacks.buildStage = [&](std::uint64_t) {
        return failsStage ? HostFrameStageStatus::Failed : HostFrameStageStatus::Ready; };
    callbacks.setCommit = [&](std::uint64_t) { state.SetTransformFrameCommit(); hasRender = true; };
    callbacks.sendRender = [&](std::uint64_t) { if (failsRender) return false; hasRender = false; return true; };
    callbacks.getRenderPending = [&] { return hasRender; };
    callbacks.sendCompletions = [] {};
    callbacks.clearStage = [&] { (void)state.ClearTransformFrame(); };
    HostFrameCoordinator frame(1,std::move(callbacks));
    (void)frame.FlushOnOwnerTick(true);
    CHECK(state.GetTransformState().modelToWorld == identity);
    failsStage = false;
    (void)frame.FlushOnOwnerTick(true);
    CHECK(state.GetTransformState().modelToWorld == matrix);
    CHECK(state.GetTransformState().completion == ModelTransformStatus::Committed);
    failsRender = false;
    (void)frame.FlushOnOwnerTick(true);
    CHECK(state.GetTransformState().modelToWorld == matrix && !hasRender);
}

void TestRotationSession();
void TestRotationHostDriven();

int main()
{
    try { TestMath(); TestState(); TestFrameFailure(); TestRotationSession(); TestRotationHostDriven(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "ModelRotation behavior passed\n";
    return 0;
}
