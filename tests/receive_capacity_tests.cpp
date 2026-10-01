#include "ProcessingElement.h"
#include <iostream>
#include <string>
static void head(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_HEAD;f.data_type=type;f.payload_data_size=size;f.command=0;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static void tail(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_TAIL;f.data_type=type;f.payload_data_size=size;f.command=0;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static Packet packet(DataType type,size_t size) {
 Packet pkt{};pkt.data_type=type;pkt.payload_data_size=size;pkt.command=0;pkt.logical_timestamp=0;return pkt;
}
// Returns true when both the advertisement and the actual commit agree.
static bool direct(ProcessingElement &p,DataType type,size_t size,bool expect) {
 Packet pkt=packet(type,size);
 const bool advertised=p.can_accept_direct_packet(pkt);
 const bool accepted=p.receive_direct_packet(pkt,1);
 std::cout<<"direct type="<<DataType_to_str(type)<<" size="<<size
          <<" advertised="<<advertised<<" accepted="<<accepted
          <<" committed="<<p.unified_buffer_manager_->GetCurrentSize(type)<<"\n";
 return advertised==expect && accepted==expect;
}
static bool reservations_zero(ProcessingElement &p) {
 return p.main_receiving_size_==0 && p.output_receiving_size_==0 &&
        p.unified_buffer_manager_!=nullptr;
}
int sc_main(int argc,char **argv) {
 GlobalParams::buffer_depth=8;GlobalParams::n_virtual_channels=2;GlobalParams::verbose_mode="OFF";
 ProcessingElement p("receiver");p.local_id=0;p.role=ROLE_GLB;p.compute_cycles=0;p.outputs_received_count_=0;
 for(auto &b:p.rx_buffer)b.SetMaxBufferSize(8);
 const std::string scenario=argc>1?argv[1]:"shared_committed";
 if(scenario=="independent" || scenario=="independent_same" || scenario=="independent_direct") {p.role=ROLE_BUFFER;p.unified_buffer_manager_=new BufferManager(std::map<DataType,size_t>{{DataType::INPUT,64},{DataType::WEIGHT,64},{DataType::OUTPUT,64}});}
 else p.unified_buffer_manager_=new BufferManager(96);
 bool ok=false;
 if(scenario=="sanity") {
  head(p,0,DataType::INPUT,32);p.internal_transfer_process();tail(p,0,DataType::INPUT,32);p.internal_transfer_process();
  ok=p.rx_buffer[0].IsEmpty()&&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==32&&p.main_receiving_size_==0;
 } else if(scenario=="direct_committed" || scenario=="direct_reserved") {
  if(scenario=="direct_committed") p.unified_buffer_manager_->OnDataReceived(DataType::INPUT,64);
  else {head(p,0,DataType::INPUT,64);p.internal_transfer_process();}
  Packet pkt{};pkt.data_type=DataType::WEIGHT;pkt.payload_data_size=64;pkt.command=0;pkt.logical_timestamp=0;
  const bool advertised=p.can_accept_direct_packet(pkt);const bool accepted=p.receive_direct_packet(pkt,1);
  std::cout<<"direct advertised="<<advertised<<" accepted="<<accepted<<" WEIGHT committed="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)<<"\n";
  ok=!advertised&&!accepted;
 } else if(scenario=="shared_committed") {
  p.unified_buffer_manager_->OnDataReceived(DataType::INPUT,64);
  head(p,0,DataType::WEIGHT,64);p.internal_transfer_process();
  const bool blocked=!p.rx_buffer[0].IsEmpty();
  std::cout<<"HEAD blocked="<<blocked<<" committed="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
  if(!blocked){tail(p,0,DataType::WEIGHT,64);p.internal_transfer_process();std::cout<<"TAIL consumed="<<p.rx_buffer[0].IsEmpty()<<" WEIGHT committed="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)<<"\n";}
  else {p.unified_buffer_manager_->RemoveData(DataType::INPUT,64);p.internal_transfer_process();tail(p,0,DataType::WEIGHT,64);p.internal_transfer_process();}
  ok=blocked&&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64;
 } else if(scenario=="interleave") {
  // Cross-VC HEAD/TAIL interleave in a shared 96B pool.
  head(p,0,DataType::INPUT,32);head(p,1,DataType::WEIGHT,32);p.internal_transfer_process();
  const bool both_heads_consumed=p.rx_buffer[0].IsEmpty()&&p.rx_buffer[1].IsEmpty();
  const bool total_reserved=p.main_receiving_size_==64;
  // Pool now has 64B inflight: only <=32B more may be advertised.
  const bool reject_33=!p.can_accept_direct_packet(packet(DataType::OUTPUT,33));
  const bool allow_32=p.can_accept_direct_packet(packet(DataType::OUTPUT,32));
  // TAIL of the INPUT packet lands while WEIGHT is still inflight.
  tail(p,0,DataType::INPUT,32);p.internal_transfer_process();
  const bool input_committed=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==32;
  const bool weight_still_reserved=p.main_receiving_size_==32;
  // 32 committed + 32 inflight + 32 new == 96: OUTPUT HEAD must fit exactly.
  head(p,0,DataType::OUTPUT,32);p.internal_transfer_process();
  const bool output_head_consumed=p.rx_buffer[0].IsEmpty()&&p.output_receiving_size_==32;
  tail(p,1,DataType::WEIGHT,32);p.internal_transfer_process();
  tail(p,0,DataType::OUTPUT,32);p.internal_transfer_process();
  const bool all_committed=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==32
                        && p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==32
                        && p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==32
                        && p.unified_buffer_manager_->GetCurrentSize()==96;
  ok=both_heads_consumed&&total_reserved&&reject_33&&allow_32&&input_committed
   &&weight_still_reserved&&output_head_consumed&&all_committed
   &&p.rx_buffer[0].IsEmpty()&&p.rx_buffer[1].IsEmpty()&&reservations_zero(p);
  std::cout<<"interleave both_heads_consumed="<<both_heads_consumed
           <<" reject_33="<<reject_33<<" allow_32="<<allow_32
           <<" final_committed="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
 } else if(scenario=="resume") {
  // Backpressure holds the HEAD; after the pool is drained it is accepted.
  p.unified_buffer_manager_->OnDataReceived(DataType::INPUT,64);
  head(p,0,DataType::WEIGHT,64);p.internal_transfer_process();
  const bool blocked=!p.rx_buffer[0].IsEmpty();
  const bool no_leak=p.main_receiving_size_==0; // blocked HEAD must not reserve
  p.unified_buffer_manager_->RemoveData(DataType::INPUT,64);
  p.internal_transfer_process(); // retry HEAD, still no TAIL queued yet
  const bool head_resumed=p.rx_buffer[0].IsEmpty()&&p.main_receiving_size_==64;
  tail(p,0,DataType::WEIGHT,64);p.internal_transfer_process();
  ok=blocked&&no_leak&&head_resumed
   &&p.rx_buffer[0].IsEmpty()
   &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
   &&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==0
   &&reservations_zero(p);
  std::cout<<"resume blocked="<<blocked<<" head_resumed="<<head_resumed
           <<" WEIGHT committed="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)<<"\n";
 } else if(scenario=="direct_mix") {
  // Direct commit must coexist with physical VC reservations in the shared pool.
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool d32=direct(p,DataType::WEIGHT,32,true);   // 0+64 inflight+32==96
  const bool d1=direct(p,DataType::OUTPUT,1,false);     // 32+64+1==97
  const bool output_untouched=p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==0;
  tail(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool input_once=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==64;
  const bool still_full=!p.can_accept_direct_packet(packet(DataType::OUTPUT,1)); // 96+0+1
  p.unified_buffer_manager_->RemoveData(DataType::WEIGHT,32);
  const bool d32_again=direct(p,DataType::OUTPUT,32,true); // 64+0+32==96
  ok=d32&&d1&&output_untouched&&input_once&&still_full&&d32_again
   &&p.unified_buffer_manager_->GetCurrentSize()==96&&reservations_zero(p);
  std::cout<<"direct_mix final committed="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
 } else if(scenario=="independent_direct") {
  // Independent pools: an INPUT reservation must not block WEIGHT/OUTPUT.
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool w_accepted=direct(p,DataType::WEIGHT,64,true);
  const bool input_full=!p.can_accept_direct_packet(packet(DataType::INPUT,1));
  const bool o_accepted=direct(p,DataType::OUTPUT,1,true); // room before TAIL
  tail(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool input_once=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==64;
  // OUTPUT pool: 1 committed now, only 63 more fit.
  const bool o64_blocked=!p.can_accept_direct_packet(packet(DataType::OUTPUT,64));
  const bool o63_accepted=direct(p,DataType::OUTPUT,63,true);
  ok=w_accepted&&input_full&&o_accepted&&input_once&&o64_blocked&&o63_accepted
   &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
   &&p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==64
   &&p.rx_buffer[0].IsEmpty()&&reservations_zero(p);
  std::cout<<"independent_direct committed INPUT="
           <<p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)
           <<" WEIGHT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)
           <<" OUTPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)<<"\n";
 } else if(scenario=="tail_commit_retry") {
  // TAIL whose commit fails must stay queued with its reservation intact;
  // it must commit exactly once after space is freed.
  head(p,0,DataType::INPUT,32);p.internal_transfer_process();
  p.unified_buffer_manager_->OnDataReceived(DataType::WEIGHT,65); // pool at 65/96
  tail(p,0,DataType::INPUT,32);p.internal_transfer_process();    // 65+32>96
  const bool tail_held=!p.rx_buffer[0].IsEmpty();
  const bool reservation_kept=p.main_receiving_size_==32;
  const bool nothing_committed=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==0;
  p.unified_buffer_manager_->RemoveData(DataType::WEIGHT,65);
  p.internal_transfer_process(); // retry the held TAIL
  const bool committed_once=p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==32;
  ok=tail_held&&reservation_kept&&nothing_committed&&committed_once
   &&p.rx_buffer[0].IsEmpty()&&reservations_zero(p);
  std::cout<<"tail_commit_retry tail_held="<<tail_held
           <<" committed_once="<<committed_once<<"\n";
 } else {
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const DataType second=scenario=="independent"?DataType::WEIGHT:scenario=="independent_same"?DataType::INPUT:DataType::OUTPUT;
  head(p,1,second,64);p.internal_transfer_process();
  const bool blocked=!p.rx_buffer[1].IsEmpty();
  std::cout<<"second HEAD blocked="<<blocked<<" INPUT reservation="<<p.main_receiving_size_<<" OUTPUT reservation="<<p.output_receiving_size_<<"\n";
  ok=scenario=="independent"?!blocked:blocked;
 }
 std::cout<<(ok?"PASS ":"FAIL ")<<scenario<<"\n";return ok?0:1;
}
