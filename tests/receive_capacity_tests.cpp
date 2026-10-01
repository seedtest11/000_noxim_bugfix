#include "ProcessingElement.h"
#include <iostream>
#include <string>
static void head(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_HEAD;f.data_type=type;f.payload_data_size=size;f.command=0;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static void tail(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_TAIL;f.data_type=type;f.payload_data_size=size;f.command=0;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
// command == -1 的输出回送包（保持特殊语义，不经 BufferManager）
static void ret_head(ProcessingElement &p,int vc,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_HEAD;f.data_type=DataType::OUTPUT;f.payload_data_size=size;f.command=-1;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static void ret_tail(ProcessingElement &p,int vc,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_TAIL;f.data_type=DataType::OUTPUT;f.payload_data_size=size;f.command=-1;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static Packet mkpacket(DataType type,size_t size,int command=0) {
 Packet pkt{};pkt.data_type=type;pkt.payload_data_size=size;pkt.command=command;pkt.logical_timestamp=0;return pkt;
}
// 所有预留最终必须归零（main=INPUT+WEIGHT，output=OUTPUT）
static bool reservations_zero(const ProcessingElement &p) {
 return p.main_receiving_size_==0 && p.output_receiving_size_==0;
}
int sc_main(int argc,char **argv) {
 GlobalParams::buffer_depth=8;GlobalParams::n_virtual_channels=2;GlobalParams::verbose_mode="OFF";
 ProcessingElement p("receiver");p.local_id=0;p.role=ROLE_GLB;p.compute_cycles=0;p.outputs_received_count_=0;
 for(auto &b:p.rx_buffer)b.SetMaxBufferSize(8);
 const std::string scenario=argc>1?argv[1]:"shared_committed";
 if(scenario=="independent" || scenario=="independent_same" || scenario=="independent_direct_other_pool") {p.role=ROLE_BUFFER;p.unified_buffer_manager_=new BufferManager(std::map<DataType,size_t>{{DataType::INPUT,64},{DataType::WEIGHT,64},{DataType::OUTPUT,64}});}
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
 } else if(scenario=="cross_vc_interleave") {
  // 共享 96：VC0 INPUT 64 与 VC1 WEIGHT 64 的 HEAD/TAIL 跨 VC 交错。
  // 第二个 HEAD 必须被整池预留阻塞；先提交第一个包后第二个仍放不下，
  // 直到腾出容量后背压恢复，TAIL 成功提交，且每包只入库一次。
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  head(p,1,DataType::WEIGHT,64);p.internal_transfer_process();
  const bool second_blocked=!p.rx_buffer[1].IsEmpty();
  // 在途预留 64 期间，任何类型超过剩余 32 的直接提交都必须被拒绝
  const bool direct_weight_blocked=!p.can_accept_direct_packet(mkpacket(DataType::WEIGHT,33));
  const bool direct_output_blocked=!p.can_accept_direct_packet(mkpacket(DataType::OUTPUT,33));
  const bool exactly_32_fits=p.can_accept_direct_packet(mkpacket(DataType::WEIGHT,32));
  // 提交 VC0 的 TAIL，容量转为已提交 64；VC1 仍然放不进 64（64+64>96）
  tail(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool still_blocked=!p.rx_buffer[1].IsEmpty();
  // 腾出 64 后，VC1 背压恢复，TAIL 成功提交
  p.unified_buffer_manager_->RemoveData(DataType::INPUT,64);
  p.internal_transfer_process();
  tail(p,1,DataType::WEIGHT,64);p.internal_transfer_process();
  std::cout<<"second_blocked="<<second_blocked<<" direct_w_blocked="<<direct_weight_blocked
           <<" direct_o_blocked="<<direct_output_blocked<<" exactly_32_fits="<<exactly_32_fits
           <<" still_blocked="<<still_blocked
           <<" INPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)
           <<" WEIGHT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)
           <<" total="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
  ok=second_blocked&&direct_weight_blocked&&direct_output_blocked&&exactly_32_fits
     &&still_blocked
     &&p.rx_buffer[0].IsEmpty()&&p.rx_buffer[1].IsEmpty()
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==0
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
     &&p.unified_buffer_manager_->GetCurrentSize()==64
     &&reservations_zero(p);
 } else if(scenario=="recover_after_free") {
  // 共享 96：INPUT 64 已提交，WEIGHT 64 HEAD 背压；逐步释放容量后恢复接收。
  p.unified_buffer_manager_->OnDataReceived(DataType::INPUT,64);
  head(p,0,DataType::WEIGHT,64);p.internal_transfer_process();
  const bool blocked=!p.rx_buffer[0].IsEmpty();
  // 只腾出 32 仍不够（64-32=32 committed，+64 = 96 <= 96? 恰好 96 -> 可接受）
  p.unified_buffer_manager_->RemoveData(DataType::INPUT,32);
  p.internal_transfer_process();
  const bool unblocked_after_32=p.rx_buffer[0].IsEmpty();
  tail(p,0,DataType::WEIGHT,64);p.internal_transfer_process();
  std::cout<<"blocked="<<blocked<<" unblocked_after_32="<<unblocked_after_32
           <<" committed="<<p.unified_buffer_manager_->GetCurrentSize()
           <<" WEIGHT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)
           <<" INPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)<<"\n";
  ok=blocked&&unblocked_after_32&&p.rx_buffer[0].IsEmpty()
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==32
     &&p.unified_buffer_manager_->GetCurrentSize()==96
     &&reservations_zero(p);
 } else if(scenario=="direct_reserved_interleave") {
  // 直接提交与物理在途预留交错（共享 96）：
  // INPUT 64 HEAD 在途 -> WEIGHT 直接 64 被拒、OUTPUT 33 被拒、32 恰好可入；
  // 物理 TAIL 提交后直接包仍须尊重已提交容量；容量腾开后同一直接包成功，
  // 且不得侵占已有预留。
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  Packet w=mkpacket(DataType::WEIGHT,64);
  const bool advertised_first=p.can_accept_direct_packet(w);
  const bool accepted_first=p.receive_direct_packet(w,1);
  const bool small_also_blocked=!p.can_accept_direct_packet(mkpacket(DataType::OUTPUT,33));
  const bool tiny_fits=p.can_accept_direct_packet(mkpacket(DataType::OUTPUT,32));
  const bool tiny_accepted=p.receive_direct_packet(mkpacket(DataType::OUTPUT,32),1);
  tail(p,0,DataType::INPUT,64);p.internal_transfer_process();
  // 此刻已提交 INPUT64+OUTPUT32=96，WEIGHT 64 无空间
  const bool still_full=!p.can_accept_direct_packet(w);
  // 全部腾空后 WEIGHT 64 直接提交成功
  p.unified_buffer_manager_->RemoveData(DataType::INPUT,64);
  p.unified_buffer_manager_->RemoveData(DataType::OUTPUT,32);
  const bool advertised_second=p.can_accept_direct_packet(w);
  const bool accepted_second=p.receive_direct_packet(w,1);
  std::cout<<"adv1="<<advertised_first<<" acc1="<<accepted_first
           <<" small_blocked="<<small_also_blocked<<" tiny_advertised="<<tiny_fits
           <<" tiny_accepted="<<tiny_accepted<<" still_full="<<still_full
           <<" adv2="<<advertised_second<<" acc2="<<accepted_second
           <<" total="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
  ok=!advertised_first&&!accepted_first&&small_also_blocked&&tiny_fits&&tiny_accepted
     &&still_full&&advertised_second&&accepted_second
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==0
     &&p.unified_buffer_manager_->GetCurrentSize()==64
     &&reservations_zero(p);
 } else if(scenario=="independent_direct_other_pool") {
  // 独立池：INPUT 64 HEAD 在途只占 INPUT 池；
  // WEIGHT/OUTPUT 直接 64 必须能进各自独立池，而同池再入 1 字节必须被阻。
  head(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool weight_direct_ok=p.receive_direct_packet(mkpacket(DataType::WEIGHT,64),1);
  const bool output_direct_ok=p.receive_direct_packet(mkpacket(DataType::OUTPUT,64),1);
  const bool input_direct_blocked=!p.can_accept_direct_packet(mkpacket(DataType::INPUT,1));
  head(p,1,DataType::INPUT,1);p.internal_transfer_process();
  const bool second_input_head_blocked=!p.rx_buffer[1].IsEmpty();
  // VC1 的 HEAD 背压保留；INPUT TAIL 先提交仍放不出 1 字节（池恰满），
  // 外部消耗 1 字节后背压恢复，再提交 VC1 的 1 字节 TAIL。
  tail(p,0,DataType::INPUT,64);p.internal_transfer_process();
  const bool vc1_still_blocked_after_tail=!p.rx_buffer[1].IsEmpty();
  p.unified_buffer_manager_->RemoveData(DataType::INPUT,1);
  p.internal_transfer_process();
  const bool head_unblocked=p.rx_buffer[1].IsEmpty();
  tail(p,1,DataType::INPUT,1);p.internal_transfer_process();
  std::cout<<"weight_direct_ok="<<weight_direct_ok<<" output_direct_ok="<<output_direct_ok
           <<" input_direct_blocked="<<input_direct_blocked
           <<" second_input_head_blocked="<<second_input_head_blocked
           <<" vc1_still_blocked_after_tail="<<vc1_still_blocked_after_tail
           <<" head_unblocked="<<head_unblocked
           <<" INPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)
           <<" WEIGHT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)
           <<" OUTPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)<<"\n";
  ok=weight_direct_ok&&output_direct_ok&&input_direct_blocked
     &&second_input_head_blocked&&vc1_still_blocked_after_tail&&head_unblocked
     &&p.rx_buffer[1].IsEmpty()
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::INPUT)==64
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==64
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==64
     &&reservations_zero(p);
 } else if(scenario=="tail_commit_retry") {
  // HEAD 已预留后、TAIL 到达前，外部把剩余容量占满：TAIL 提交必须失败，
  // 队首保留、预留不释放、不丢包；腾出容量后重试成功，每包只入库一次。
  head(p,0,DataType::WEIGHT,32);p.internal_transfer_process();
  // 在途 32，已提交 0；外部占用 65（>64 剩余）使 32 的 TAIL 提交失败
  p.unified_buffer_manager_->OnDataReceived(DataType::OUTPUT,65);
  tail(p,0,DataType::WEIGHT,32);p.internal_transfer_process();
  const bool tail_retained=!p.rx_buffer[0].IsEmpty();
  const size_t reservation_after_fail=p.main_receiving_size_;
  const size_t weight_after_fail=p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT);
  // 重试一次仍然失败，且不重复入库
  p.internal_transfer_process();
  const bool still_retained=!p.rx_buffer[0].IsEmpty();
  // 腾出 1 后恰可提交 32（65-1=64 committed + 32 = 96）
  p.unified_buffer_manager_->RemoveData(DataType::OUTPUT,1);
  p.internal_transfer_process();
  std::cout<<"tail_retained="<<tail_retained<<" reservation_after_fail="<<reservation_after_fail
           <<" weight_after_fail="<<weight_after_fail<<" still_retained="<<still_retained
           <<" WEIGHT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)
           <<" OUTPUT="<<p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)
           <<" total="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
  ok=tail_retained&&reservation_after_fail==32&&weight_after_fail==0
     &&still_retained&&p.rx_buffer[0].IsEmpty()
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::WEIGHT)==32
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==64
     &&p.unified_buffer_manager_->GetCurrentSize()==96
     &&reservations_zero(p);
 } else if(scenario=="return_command_special") {
  // command == -1 输出回送：HEAD/TAIL 不经 BufferManager、不占容量，
  // 即使池满也接受，只累加 outputs_received_count_。
  p.unified_buffer_manager_->OnDataReceived(DataType::INPUT,64);
  p.unified_buffer_manager_->OnDataReceived(DataType::WEIGHT,32);
  ret_head(p,0,16);p.internal_transfer_process();
  ret_tail(p,0,16);p.internal_transfer_process();
  const bool direct_return_ok=p.receive_direct_packet(mkpacket(DataType::OUTPUT,8,-1),1);
  std::cout<<"head_consumed return outputs="<<p.outputs_received_count_
           <<" direct_return_ok="<<direct_return_ok
           <<" OUTPUT_committed="<<p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)
           <<" total="<<p.unified_buffer_manager_->GetCurrentSize()<<"\n";
  ok=p.rx_buffer[0].IsEmpty()&&p.outputs_received_count_==24&&direct_return_ok
     &&p.unified_buffer_manager_->GetCurrentSize(DataType::OUTPUT)==0
     &&p.unified_buffer_manager_->GetCurrentSize()==96
     &&reservations_zero(p);
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
