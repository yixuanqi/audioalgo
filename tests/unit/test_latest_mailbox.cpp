#include "audio_editor_application/LatestRequestMailbox.h"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

namespace {struct Payload {int value;};}

TEST_CASE("AR-17.a mailbox releases replaced payloads and ignores stale worker cleanup","[AR-17.a][pure]") {
    audioalgo::LatestRequestMailbox<std::shared_ptr<Payload>> mailbox;
    std::vector<std::weak_ptr<Payload>> observers;
    const auto submit=[&](int value) {
        auto payload=std::make_shared<Payload>(value);observers.emplace_back(payload);
        return mailbox.submit(std::move(payload));
    };
    const auto first=submit(0);REQUIRE(first.accepted);REQUIRE(first.start_worker);
    auto active=mailbox.take(first.ticket);REQUIRE(active);
    for(int i=1;i<=50;++i) {
        const auto pending=submit(i);CHECK(pending.accepted);CHECK_FALSE(pending.start_worker);
        std::size_t alive=0;for(const auto& observer:observers)alive+=!observer.expired();
        CHECK(alive==2);CHECK(active->cancelled->load());
        const auto counts=mailbox.counts();CHECK(counts.active==1);CHECK(counts.pending==1);
    }
    active.reset();
    for(std::size_t i=0;i<50;++i)CHECK(observers[i].expired());
    auto latest=mailbox.take(first.ticket);REQUIRE(latest);CHECK(latest->request->value==50);CHECK_FALSE(latest->cancelled->load());
    latest.reset();CHECK_FALSE(mailbox.take(first.ticket));
    const auto new_worker=submit(51);CHECK(new_worker.start_worker);CHECK(new_worker.ticket!=first.ticket);
    mailbox.abandon(first.ticket); // A finishing old job must not delete the new worker's request.
    const auto next=mailbox.take(new_worker.ticket);REQUIRE(next);CHECK(next->request->value==51);
    mailbox.cancel(true);CHECK(next->cancelled->load());
    mailbox.abandon(new_worker.ticket);CHECK_FALSE(mailbox.counts().worker_claimed);
    CHECK_FALSE(submit(52).accepted);CHECK(observers.back().expired());
}

TEST_CASE("AR-17.a mailbox cancels an old session while accepting the new latest snapshot","[AR-17.a][pure]") {
    audioalgo::LatestRequestMailbox<int> mailbox;
    const auto worker=mailbox.submit(1);auto active=mailbox.take(worker.ticket);REQUIRE(active);
    CHECK(mailbox.submit(2).accepted);mailbox.cancel();
    CHECK(active->cancelled->load());CHECK(mailbox.counts().pending==0);
    const auto replacement=mailbox.submit(3);CHECK(replacement.accepted);CHECK_FALSE(replacement.start_worker);
    active.reset();const auto next=mailbox.take(worker.ticket);REQUIRE(next);CHECK(next->request==3);CHECK_FALSE(next->cancelled->load());
    mailbox.cancel(true);CHECK(next->cancelled->load());mailbox.abandon(worker.ticket);
    CHECK(mailbox.counts().active==0);CHECK(mailbox.counts().pending==0);CHECK_FALSE(mailbox.counts().worker_claimed);
}
