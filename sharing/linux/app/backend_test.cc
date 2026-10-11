#include "sharing/linux/app/backend.h"

#include <cstdlib>
#include <memory>

#include <QCoreApplication>
#include <QEventLoop>
#include "gtest/gtest.h"
#include "sharing/linux/app/fake_quickshare.h"

class BackendTestPeer {
 public:
  static std::unique_ptr<Backend> Create(QuickShare* qs) {
    return std::unique_ptr<Backend>(
        new Backend(qs, /*receive_timeout_milliseconds=*/5000, nullptr));
  }

  static bool IsIdle(const Backend& backend) {
    return backend.active_mode_ == Backend::Mode::kNone;
  }

  static bool IsReceiving(const Backend& backend) {
    return backend.active_mode_ == Backend::Mode::kReceive;
  }

  static bool IsDiscovering(const Backend& backend) {
    return backend.active_mode_ == Backend::Mode::kDiscovery;
  }

  static bool IsReceiveTimerActive(const Backend& backend) {
    return backend.receive_timeout_timer_.isActive();
  }

  static bool IsMonitoringRequested(const Backend& backend) {
    return backend.monitoring_requested_;
  }

  static bool HasIncomingOfferTimer(const Backend& backend,
                                    int64_t share_target_id) {
    return backend.incoming_offer_timers_.contains(share_target_id);
  }

  static void ExpireReceiveWindow(Backend& backend) {
    backend.OnReceiveTimeout();
  }

  static void ExpireIncomingOffer(Backend& backend, int64_t share_target_id) {
    backend.OnIncomingOfferTimeout(share_target_id);
  }
};

namespace {

QCoreApplication& TestApplication() {
  static int argc = 1;
  static char application_name[] = "backend_test";
  static char* argv[] = {application_name, nullptr};
  static QCoreApplication application(argc, argv);
  return application;
}

void DrainEvents() {
  static_cast<void>(TestApplication());
  for (int i = 0; i < 4; ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  }
}

class BackendFastInitiationTest : public testing::Test {
 protected:
  void SetUp() override {
    static_cast<void>(TestApplication());
    qs_ = quickshare_create();
    backend_ = BackendTestPeer::Create(qs_);
  }

  void TearDown() override {
    backend_.reset();
    DrainEvents();
  }

  QuickShare* qs_ = nullptr;
  std::unique_ptr<Backend> backend_;
};

TEST_F(BackendFastInitiationTest, StartupScansWithoutReceiving) {
  EXPECT_TRUE(qs_->scanning);
  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));
  EXPECT_FALSE(BackendTestPeer::IsReceiveTimerActive(*backend_));
}

TEST_F(BackendFastInitiationTest, PresenceOpensTimedReceiveWindow) {
  qs_->FireDiscovered();
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_TRUE(BackendTestPeer::IsReceiveTimerActive(*backend_));
  EXPECT_TRUE(qs_->scanning);
}

TEST_F(BackendFastInitiationTest,
       TimeoutRequiresSenderAbsenceBeforeAnotherWindow) {
  qs_->FireDiscovered();
  DrainEvents();
  BackendTestPeer::ExpireReceiveWindow(*backend_);
  DrainEvents();
  ASSERT_TRUE(BackendTestPeer::IsIdle(*backend_));

  qs_->FireDiscovered();
  DrainEvents();
  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));

  qs_->FireNotDiscovered();
  qs_->FireDiscovered();
  DrainEvents();
  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
}

TEST_F(BackendFastInitiationTest, SenderLossDoesNotCloseCurrentWindow) {
  qs_->FireDiscovered();
  DrainEvents();
  qs_->FireNotDiscovered();
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_TRUE(BackendTestPeer::IsReceiveTimerActive(*backend_));
}

TEST_F(BackendFastInitiationTest, IncomingOfferCancelsReceiveTimeout) {
  qs_->FireDiscovered();
  DrainEvents();

  const int64_t target_id = 1;
  qs_->FireReceiveTransfer(target_id, QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION);
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_FALSE(BackendTestPeer::IsReceiveTimerActive(*backend_));
  EXPECT_TRUE(BackendTestPeer::HasIncomingOfferTimer(*backend_, target_id));
}

TEST_F(BackendFastInitiationTest, AcceptCancelsIncomingOfferTimeout) {
  qs_->FireDiscovered();
  DrainEvents();

  const int64_t target_id = 11;
  qs_->FireReceiveTransfer(target_id, QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION);
  DrainEvents();

  backend_->accept(target_id);

  EXPECT_FALSE(BackendTestPeer::HasIncomingOfferTimer(*backend_, target_id));
  EXPECT_EQ(qs_->accept_count, 1);
  EXPECT_EQ(qs_->last_accepted_target, target_id);
  BackendTestPeer::ExpireIncomingOffer(*backend_, target_id);
  EXPECT_EQ(qs_->reject_count, 0);
}

TEST_F(BackendFastInitiationTest, IncomingOfferTimeoutRejectsExactlyOnce) {
  qs_->FireDiscovered();
  DrainEvents();

  const int64_t target_id = 12;
  qs_->FireReceiveTransfer(target_id, QUICKSHARE_TRANSFER_AWAITING_LOCAL_CONFIRMATION);
  DrainEvents();

  BackendTestPeer::ExpireIncomingOffer(*backend_, target_id);
  BackendTestPeer::ExpireIncomingOffer(*backend_, target_id);

  EXPECT_FALSE(BackendTestPeer::HasIncomingOfferTimer(*backend_, target_id));
  EXPECT_EQ(qs_->reject_count, 1);
  EXPECT_EQ(qs_->last_rejected_target, target_id);
}

TEST_F(BackendFastInitiationTest, FinalReceiveRestartsMonitoring) {
  qs_->FireDiscovered();
  DrainEvents();

  const int64_t target_id = 13;
  qs_->FireReceiveTransfer(target_id, QUICKSHARE_TRANSFER_COMPLETE);
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));
  EXPECT_TRUE(BackendTestPeer::IsMonitoringRequested(*backend_));
  EXPECT_TRUE(qs_->scanning);
}

TEST_F(BackendFastInitiationTest, DiscoveryStopsFastInitiationScanning) {
  backend_->startDiscovery();
  EXPECT_FALSE(qs_->scanning);
  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));

  qs_->CompleteStop();
  DrainEvents();
  EXPECT_TRUE(BackendTestPeer::IsDiscovering(*backend_));

  backend_->startReceive();
  DrainEvents();
  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));
  EXPECT_TRUE(qs_->scanning);
}

TEST_F(BackendFastInitiationTest, ScanErrorUsesOneFallbackWindowThenIdles) {
  quickshare_fast_init_stop_scanning(qs_, nullptr, nullptr);
  qs_->FireError(/*kHardwareNotSupported=*/5);
  DrainEvents();

  ASSERT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_TRUE(BackendTestPeer::IsReceiveTimerActive(*backend_));
  BackendTestPeer::ExpireReceiveWindow(*backend_);
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));
  EXPECT_FALSE(BackendTestPeer::IsMonitoringRequested(*backend_));
  EXPECT_FALSE(qs_->scanning);
}

}  // namespace

TEST_F(BackendFastInitiationTest, ReceiveEnabledStaysReceivingWithoutTimeout) {
  backend_->setReceiveEnabled(true);
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_FALSE(BackendTestPeer::IsReceiveTimerActive(*backend_));
}

TEST_F(BackendFastInitiationTest, ReceiveDisabledReturnsToFastInitiation) {
  backend_->setReceiveEnabled(true);
  DrainEvents();
  backend_->setReceiveEnabled(false);
  DrainEvents();

  EXPECT_TRUE(BackendTestPeer::IsIdle(*backend_));
  EXPECT_TRUE(qs_->scanning);
}

TEST_F(BackendFastInitiationTest, ReceiveEnabledResumesAfterDiscovery) {
  backend_->setReceiveEnabled(true);
  DrainEvents();
  backend_->startDiscovery();
  qs_->CompleteStop();
  DrainEvents();
  ASSERT_TRUE(BackendTestPeer::IsDiscovering(*backend_));

  backend_->startReceive();
  DrainEvents();
  EXPECT_TRUE(BackendTestPeer::IsReceiving(*backend_));
  EXPECT_FALSE(BackendTestPeer::IsReceiveTimerActive(*backend_));
}
