#include "luca/adapters/transaction.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace luca;
namespace {
std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> result;
  std::size_t start=0;
  while (true) { const auto end=s.find('\t',start); result.push_back(s.substr(start,end-start));
    if (end==std::string::npos) return result; start=end+1; }
}
std::string line() { std::string s; if (!std::getline(std::cin,s) || s.size()>4*1024*1024) throw std::runtime_error("protocol_input"); return s; }
std::string unhex(const std::string& s) {
  if (s.size()%2) throw std::runtime_error("protocol_hex");
  std::string r; r.reserve(s.size()/2);
  auto digit=[](char c)->unsigned { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; throw std::runtime_error("protocol_hex"); };
  for (std::size_t i=0;i<s.size();i+=2) r.push_back(static_cast<char>((digit(s[i])<<4)|digit(s[i+1])));
  return r;
}
std::string hex(std::string_view s) { const char* digits="0123456789abcdef"; std::string r;
  for (unsigned char c:s) { r+=digits[c>>4]; r+=digits[c&15]; } return r; }
std::string bytes_hex(std::span<const std::byte> s) { return hex({reinterpret_cast<const char*>(s.data()),s.size()}); }
adapters::TransactionFields fields() {
  auto f=split(line()); if(f.size()!=16) throw std::runtime_error("protocol_fields");
  for(auto& value:f) value=unhex(value);
  return {f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]};
}
}
int main() {
  try {
    std::cout<<std::unitbuf<<"LUCA_INGESTION\t1\tluca/0.1\n";
    LifecycleLedger ledger;
    std::optional<PreparedLifecycleAppend> pending;
    std::string command;
    while(std::getline(std::cin,command)) {
      auto parts=split(command);
      const auto op=parts.front();
      if(op=="C") {
        if(!pending) throw std::runtime_error("protocol_no_preparation");
        auto accepted=ledger.publish(std::move(*pending)); pending.reset();
        if(!accepted) throw std::runtime_error("protocol_stale_preparation");
        std::cout<<"OK\n"; continue;
      }
      if(op=="X") { pending.reset(); std::cout<<"OK\n"; continue; }
      if(parts.size()!=2) throw std::runtime_error("protocol_count");
      unsigned count=0; auto parsed=std::from_chars(parts[1].data(),parts[1].data()+parts[1].size(),count);
      if(parsed.ec!=std::errc{} || parsed.ptr!=parts[1].data()+parts[1].size() || count>1000) throw std::runtime_error("protocol_count");
      if(op=="R") {
        if(pending) throw std::runtime_error("protocol_pending");
        for(unsigned i=0;i<count;++i) {
          const auto data=unhex(line());
          auto decoded=serialization::decode_lifecycle_record({reinterpret_cast<const std::byte*>(data.data()),data.size()});
          if(!decoded || decoded->acceptance_sequence!=ledger.size()+1) throw std::runtime_error("corrupt_canonical_log");
          auto accepted=ledger.accept(decoded->draft);
          if(!accepted || bytes_hex(serialization::canonical_bytes(accepted->get()))!=hex(data)) throw std::runtime_error("corrupt_canonical_log");
        }
        std::cout<<"OK\n"; continue;
      }
      if(op!="V" && op!="P") throw std::runtime_error("protocol_command");
      std::vector<std::expected<LifecycleRecordDraft,adapters::TransactionDiagnostic>> values;
      for(unsigned i=0;i<count;++i) values.push_back(adapters::transaction(fields()));
      if(op=="V") {
        for(const auto& value:values) {
          if(value) std::cout<<"OK\n";
          else std::cout<<"INVALID\t"<<value.error().code<<'\t'<<hex(value.error().field)<<'\t'<<hex(value.error().message)<<'\n';
        }
      } else {
        pending.reset(); std::vector<LifecycleRecordDraft> drafts;
        for(const auto& value:values) { if(!value) throw std::runtime_error("protocol_invalid_candidate"); drafts.push_back(*value); }
        auto prepared=ledger.prepare_batch(drafts);
        if(!prepared) { const auto& e=prepared.error(); std::cout<<"ERROR\t"<<e.category_name()<<'\t'<<hex(e.record_id().value())<<'\t'<<hex(e.causal_record_id() ? e.causal_record_id()->value() : "")<<'\t'<<hex(e.message())<<'\n'; }
        else { pending=std::move(*prepared); std::cout<<"READY\t"<<pending->records().size()<<'\n';
          for(const auto& record:pending->records()) std::cout<<record.acceptance_sequence().value()<<'\t'<<bytes_hex(serialization::canonical_bytes(record))<<'\n'; }
      }
    }
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; }
}
