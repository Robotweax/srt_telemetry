#include <numeric_input_draft.hpp>
#include <iostream>
namespace d=robotweax::srt::telemetry::remote_draft;
int main() {
 d::Context c; c.segment_id="segment";c.producer_id="producer";c.source_id="source";c.role=d::EndpointRole::sender;c.observer_clock_domain="cloud";c.origin={"fixture","1",{}};
 d::NumericQualification q;q.id="fixture";q.provider=ROBOTWEAX_SRT_TELEMETRY_PROVIDER_ROBOTWEAX_SRT;q.since_clear=d::SinceClearPolicy::exclusive_no_clear;q.extended_fields=ROBOTWEAX_SRT_TELEMETRY_EXT_FIELD_RECEIVED_RETRANSMITTED_PACKET_SINCE_CLEAR;
 d::NumericProcessor p(c,q);d::NumericValues v;v.extended.available_fields=q.extended_fields;v.clear_boundary=0;v.extended.received_retransmitted_packet_since_clear=10;
 p.push({1,1000,{},{},{1}},{1,{}},v);
 v.clear_boundary=1;v.extended.received_retransmitted_packet_since_clear=100;
 auto r=p.push({2,2000,{},{},{2}},{2,{}},v);
 std::cout<<"accepted="<<(r.status==d::PushResult::accepted)<<" since_clear_delta="<<r.sample->extended_delta.received_retransmitted_packet_delta<<" boundary_changed="<<r.sample->clear_boundary_changed<<'\n';
}
