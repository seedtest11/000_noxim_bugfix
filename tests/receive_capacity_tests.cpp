#include "ProcessingElement.h"
#include <iostream>
#include <string>
static void head(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_HEAD;f.data_type=type;f.payload_data_size=size;f.command=0;f.logical_timestamp=0;p.rx_buffer[vc].Push(f);
}
static void tail(ProcessingElement &p,int vc,DataType type,size_t size) {
 Flit f{};f.vc_id=vc;f.flit_type=FLIT_TYPE_TAIL;f.data_type=type;f.payload_data_size=size;f.command=0;p.rx_buffer[vc].Push(f);
}
int sc_main(int argc,char **argv) {
 GlobalParams::buffer_depth=8;GlobalParams::n_virtual_channels=2;GlobalParams::verbose_mode="OFF";
 ProcessingElement p("receiver");p.local_id=0;p.role=ROLE_GLB;p.compute_cycles=0;p.outputs_received_count_=0;
 for(auto &b:p.rx_buffer)b.SetMaxBufferSize(8);
 const std::string scenario=argc>1?argv[1]:"shared_committed";
 if(scenario=="independent" || scenario=="independent_same") {p.role=ROLE_BUFFER;p.unified_buffer_manager_=new BufferManager(std::map<DataType,size_t>{{DataType::INPUT,64},{DataType::WEIGHT,64},{DataType::OUTPUT,64}});}
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
