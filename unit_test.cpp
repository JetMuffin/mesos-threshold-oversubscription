#include "threshold_resource_estimator.hpp"

#include <stout/os.hpp>

#include <gtest/gtest.h>


using process::Future;

using mesos::Resources;
using mesos::ResourceUsage;

using com::blue_yonder::ThresholdResourceEstimator;

namespace {

class UsageMock {
public:
    UsageMock()
        : value{std::make_shared<ResourceUsage>()}
    {};

    void set(std::string const & revocable_allocated, std::string const & non_revocable_allocated) {
        value->Clear();
        auto * revocable_executor = value->add_executors();
        auto revocable_resources = Resources::parse(revocable_allocated);
        for(auto const & parsed_resource: revocable_resources.get()) {
            auto * mutable_resource = revocable_executor->add_allocated();
            mutable_resource->CopyFrom(parsed_resource);
            mutable_resource->mutable_revocable();  // mark as revocable
        }

        auto * non_revocable_executor = value->add_executors();
        auto non_revocable_resources = Resources::parse(non_revocable_allocated);
        for(auto const & parsed_resource: non_revocable_resources.get()) {
            auto * mutable_resource = non_revocable_executor->add_allocated();
            mutable_resource->CopyFrom(parsed_resource);
            mutable_resource->clear_revocable();  // mark as non-revocable
        }
    }
    Future<ResourceUsage> operator()() const {
        return *value;
    }
private:
    std::shared_ptr<ResourceUsage> value;
};

class LoadMock {
public:
    LoadMock() : value{std::make_shared<os::Load>()} {};

    Try<os::Load> operator()() {
        return *value;
    }

    void set(float one, float five, float fifteen) {
        *value = os::Load{one, five, fifteen};
    }

private:
    std::shared_ptr<os::Load> value;
};

#define EXPECT_LOAD(expect_one, expect_five, expect_fifteen, load) \
    do { \
        EXPECT_EQ(expect_one, load.one); \
        EXPECT_EQ(expect_five, load.five); \
        EXPECT_EQ(expect_fifteen, load.fifteen); \
    } while(false); \

TEST(UsageMockTest, test_set) {
    UsageMock mock;
    mock.set("cpus:3;mem:62", "cpus:1;mem:48");

    ResourceUsage usage = mock().get();
    Resources allocatedRevocable;
    Resources allocatedNonRevocable;
    for(auto & executor: usage.executors()) {
        allocatedRevocable += Resources(executor.allocated()).revocable();
        allocatedNonRevocable += Resources(executor.allocated()).nonRevocable();
    }

    EXPECT_EQ(2, usage.executors_size());
    EXPECT_EQ(3, allocatedRevocable.cpus().get());
    EXPECT_EQ(Bytes::parse("62MB").get(), allocatedRevocable.mem().get());
    EXPECT_EQ(1, allocatedNonRevocable.cpus().get());
    EXPECT_EQ(Bytes::parse("48MB").get(), allocatedNonRevocable.mem().get());

    // must also work for copy
    UsageMock copy = mock;
    mock.set("cpus:5;mem:127", "");
    usage = copy().get();

    Resources copiedRevocable;
    for(auto & executor: usage.executors()) {
        copiedRevocable += Resources(executor.allocated()).revocable();
    }
    EXPECT_EQ(5, copiedRevocable.cpus().get());
    EXPECT_EQ(Bytes::parse("127MB").get(), copiedRevocable.mem().get());
}

TEST(LoadMockTest, test_set) {
    LoadMock mock;
    EXPECT_LOAD(0, 0, 0, mock().get());

    auto copy = mock;
    mock.set(1.5, 2.5, 3.5);
    EXPECT_LOAD(1.5, 2.5, 3.5, mock().get());
    EXPECT_LOAD(1.5, 2.5, 3.5, copy().get());
}

TEST(ThresholdResourceEstimatorTest, test_noop) {
    UsageMock usage;
    LoadMock load;
    ThresholdResourceEstimator estimator{load, Resources::parse("").get(), None(), None(), None()};
    estimator.initialize(usage);
    auto available_resources = estimator.oversubscribable().get();
    EXPECT_TRUE(available_resources.empty());
}

TEST(ThresholdResourceEstimatorTest, test_underutilized) {
    UsageMock usage;  // no usage at all
    LoadMock load;
    ThresholdResourceEstimator estimator{load, Resources::parse("cpus(*):2;mem(*):512").get(), None(), None(), None()};
    estimator.initialize(usage);
    auto available_resources = estimator.oversubscribable().get();
    EXPECT_FALSE(available_resources.empty());
    EXPECT_EQ(2.0, available_resources.revocable().cpus().get());
    EXPECT_EQ(Bytes::parse("512MB").get(), available_resources.revocable().mem().get());

    // Usage of revocable resources reduces offers
    usage.set("cpus(*):1.5;mem(*):128", "");
    available_resources = estimator.oversubscribable().get();
    EXPECT_EQ(0.5, available_resources.revocable().cpus().get());
    EXPECT_EQ(Bytes::parse("384MB").get(), available_resources.revocable().mem().get());

    // Usage of non-revocable resources is ignored
    usage.set("", "cpus(*):1.5;mem(*):128");
    available_resources = estimator.oversubscribable().get();
    EXPECT_EQ(2.0, available_resources.revocable().cpus().get());
    EXPECT_EQ(Bytes::parse("512MB").get(), available_resources.revocable().mem().get());
}

TEST(ThresholdResourceEstimatorTest, test_load_threshold_exceeded) {
    UsageMock usage;  // no usage at all
    LoadMock load;
    load.set(3.9, 2.9, 1.9);
    ThresholdResourceEstimator estimator{
        load,
        Resources::parse("cpus(*):2;mem(*):512").get(),
        4,
        3,
        2
    };
    estimator.initialize(usage);

    // test _not_ exceeded
    auto available_resources = estimator.oversubscribable().get();
    EXPECT_FALSE(available_resources.empty());
    EXPECT_EQ(2.0, available_resources.revocable().cpus().get());
    EXPECT_EQ(Bytes::parse("512MB").get(), available_resources.revocable().mem().get());

    // test 1 minute threshold exceeded
    load.set(4.0, 2.9, 1.9);
    available_resources = estimator.oversubscribable().get();
    EXPECT_TRUE(available_resources.empty());

    // test 5 minute threshold exceeded
    load.set(3.9, 3.0, 1.9);
    available_resources = estimator.oversubscribable().get();
    EXPECT_TRUE(available_resources.empty());

    // test 15 minute threshold exceeded
    load.set(3.9, 2.9, 2.0);
    available_resources = estimator.oversubscribable().get();
    EXPECT_TRUE(available_resources.empty());
}

}
