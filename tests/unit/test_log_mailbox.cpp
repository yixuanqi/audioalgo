#include "audio_editor_application/RunLogMailbox.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("PC-04.d pending log delivery bounds owned bytes records and scheduled callbacks", "[PC-04.d][pure]") {
    using namespace audioalgo;
    editor::RunLogMailbox mailbox;
    NodeLogEvent event;event.identity={"project",3,"request",7};event.node_id="node";event.text=std::string(8192,'x');
    int scheduled=0;
    for(int i=0;i<1000;++i) {event.sequence=i+1;scheduled+=mailbox.push(event);}
    CHECK(scheduled==1);CHECK(mailbox.counts().bytes<=256*1024);CHECK(mailbox.counts().events<=128);
    const auto batch=mailbox.take();REQUIRE_FALSE(batch.empty());CHECK(batch.back().sequence==1000);CHECK(batch.back().identity==event.identity);
    CHECK(batch.back().ui_dropped_events>0); CHECK(batch.back().ui_dropped_text_bytes>0);
    CHECK(mailbox.counts().bytes==0);CHECK(mailbox.counts().events==0);CHECK_FALSE(mailbox.counts().scheduled);
    event.text.clear();scheduled=0;
    for(int i=0;i<1000;++i)scheduled+=mailbox.push(event);
    CHECK(scheduled==1);CHECK(mailbox.counts().bytes<=256*1024);CHECK(mailbox.counts().events<=128);
}
