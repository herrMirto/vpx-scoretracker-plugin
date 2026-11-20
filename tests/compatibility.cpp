// license:GPLv3+
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "plugins/ControllerPlugin.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/VPXPlugin.h"
#include "plugins/LoggingPlugin.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;
static std::map<std::string, unsigned> ids;
static std::map<unsigned, int> refs;
using Subscription = std::tuple<uint32_t, unsigned, msgpi_msg_callback, void*>;
static std::vector<Subscription> subscriptions;
static VPXPluginAPI vpx{};
static ScriptablePluginAPI script{};
static LoggingPluginAPI logging{};
static ScriptClassDef* recorder = nullptr;
static std::string tablePath, output;
static bool uiReady = false, notificationsEnabled = true, sourceLive = true;
static unsigned reads = 0;
static bool hostGameActive = false;
static std::vector<std::string> notices, logs;
// Deliberately unordered: player order must follow labels, not enumeration.
static int64_t values[] = {0,0,0,0,0,0,3,100};
static void GetState(void* context, void* result) {
   assert(sourceLive); ++reads; *static_cast<int64_t*>(result) = *static_cast<int64_t*>(context);
}
static StateDef states[] = {
   {"Player 2", "game_state\\scores\\Player 2", 0, CTLPI_STATE_FORMAT_INT64, 0, &values[5], GetState, nullptr},
   {"Game Over", "game_state\\game_over\\Game Over", 1, CTLPI_STATE_FORMAT_INT64, 0, &values[0], GetState, nullptr},
   {"Players", "game_state\\player_count\\Players", 2, CTLPI_STATE_FORMAT_INT64, 0, &values[1], GetState, nullptr},
   {"Current Player", "game_state\\current_player\\Current Player", 3, CTLPI_STATE_FORMAT_INT64, 0, &values[2], GetState, nullptr},
   {"Ball", "game_state\\current_ball\\Ball", 4, CTLPI_STATE_FORMAT_INT64, 0, &values[3], GetState, nullptr},
   {"Player 1", "game_state\\scores\\Player 1", 5, CTLPI_STATE_FORMAT_INT64, 0, &values[4], GetState, nullptr},
   {"Ball Count", "game_state\\ball_count\\Ball Count", 6, CTLPI_STATE_FORMAT_INT64, 0, &values[6], GetState, nullptr},
   {"Games Started", "audits\\B.3 Standard Audits\\01\\Games Started", 7, CTLPI_STATE_FORMAT_INT64, 0, &values[7], GetState, nullptr}
};
static unsigned GetId(const char* ns, const char* name) {
   const std::string key = std::string(ns) + '/' + name;
   assert(!key.starts_with("ScoreCapture/")); // No invented PinMAME extension is allowed.
   const auto [entry, inserted] = ids.emplace(key, unsigned(ids.size()+1)); ++refs[entry->second]; return entry->second;
}
static bool IsId(unsigned id, const std::string& key) {
   const auto it = ids.find(key); return it != ids.end() && it->second == id;
}
static void Broadcast(uint32_t, unsigned id, void* data) {
   if (IsId(id,"VPX/GetAPI:1")) *static_cast<VPXPluginAPI**>(data) = &vpx;
   if (IsId(id,"Scriptable/GetAPI:1")) *static_cast<ScriptablePluginAPI**>(data) = &script;
   if (IsId(id,"Login/GetAPI:1")) *static_cast<LoggingPluginAPI**>(data) = &logging;
   if (IsId(id,"Controller/" CTLPI_CONTROLLERS_GET_MSG)) {
      auto& msg = *static_cast<GetControllersMsg*>(data);
      if (msg.maxEntryCount) msg.entries[0] = {42,"pinmame::afm_113b"}; msg.count = 1;
   }
   if (IsId(id,"Controller/" CTLPI_STATE_GET_SRC_MSG)) {
      auto& msg = *static_cast<GetStateSrcMsg*>(data); msg.count = sourceLive ? 1 : 0;
      if (sourceLive && msg.maxEntryCount) {
         StateSrcId source{}; source.id.endpointId=42; source.id.resId=7;
         source.nStates=8; source.stateDefs=states; msg.entries[0]=source;
      }
   }
}
static void Event(const std::string& name) {
   if (name == "VPX/OnGameStart:1") hostGameActive = true;
   const auto current = subscriptions;
   for (const auto& [ep,id,fn,ctx] : current) if (IsId(id,name)) fn(id,ctx,nullptr);
   if (name == "VPX/OnGameEnd:1") hostGameActive = false;
}
static void Frame() { Event("VPX/OnPrepareFrame:1"); }
static nlohmann::json History() {
   std::ifstream in(fs::path(output)/"scores.json"); return nlohmann::json::parse(in);
}
static size_t Games() { return fs::exists(fs::path(output)/"scores.json") ? History()["games"].size() : 0; }
static size_t SavedNotices() {
   return std::count_if(notices.begin(),notices.end(),[](const auto& msg){ return msg=="ScoreTracker: score saved."; });
}
static ScriptVariant Call(void* object, const char* name, ScriptVariant* args=nullptr) {
   ScriptVariant result{};
   for (unsigned i=0;i<recorder->nMembers;++i) if (std::strcmp(recorder->members[i].name.name,name)==0) {
      recorder->members[i].Call(object,int(i),args,&result); return result;
   }
   assert(false); return result;
}
static void Idle() { values[0]=0; values[2]=values[3]=0; Frame(); }
static void Start(unsigned players=1) {
   ++values[7]; values[0]=0; values[1]=players; values[2]=values[3]=1;
   values[4]=values[5]=0; Frame();
}
static void Complete(int64_t score, unsigned players=1) {
   values[0]=0; values[3]=3; values[2]=players; Frame();
   values[4]=score; values[5]=42; values[0]=1; Frame();
}
int main(int argc,char** argv) {
   assert(argc==3);
   const auto root=fs::path(argv[2])/("native-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   MsgPluginAPI api{}; api.GetMsgID=GetId;
   api.ReleaseMsgID=[](unsigned id){assert(refs[id]>0);--refs[id];};
   api.BroadcastMsg=Broadcast;
   api.SendMsg=[](uint32_t,unsigned,uint32_t,void*){assert(false);}; // No custom provider request or ROM path query.
   api.SubscribeMsg=[](uint32_t ep,unsigned id,msgpi_msg_callback fn,void* ctx){subscriptions.emplace_back(ep,id,fn,ctx);};
   api.UnsubscribeMsg=[](unsigned id,msgpi_msg_callback fn,void* ctx){
      const auto it=std::find_if(subscriptions.begin(),subscriptions.end(),[&](const auto& s){return std::get<1>(s)==id&&std::get<2>(s)==fn&&std::get<3>(s)==ctx;});
      assert(it!=subscriptions.end());subscriptions.erase(it);
   };
   api.GetEndpointInfo=[](unsigned,MsgEndpointInfo* info){info->id="ScoreTracker";};
   api.RegisterSetting=[](uint32_t,MsgSettingDef* setting){
      if(setting->type==MSGPI_SETTING_TYPE_STRING)setting->stringDef.Set(output.c_str());
      else if(setting->type==MSGPI_SETTING_TYPE_BOOL)setting->boolDef.Set(notificationsEnabled);
      else assert(false);
   };
   api.RunOnMainThread=[](uint32_t,double,msgpi_timer_callback,void*){assert(false);};
   api.FlushPendingCallbacks=[](uint32_t){};
   vpx.GetTableInfo=[](VPXTableInfo* info){assert(hostGameActive);info->path=tablePath.c_str();};
   vpx.PushNotification=[](const char* text,int)->unsigned{if(!uiReady)return 0;notices.emplace_back(text);return unsigned(notices.size());};
   logging.version=1; logging.Log=[](const char*,const char*,int,unsigned,const char* text){logs.emplace_back(text);};
   script.RegisterScriptClass=[](ScriptClassDef* def){recorder=def;};
   script.UnregisterScriptClass=[](ScriptClassDef* def){assert(def==recorder);recorder=nullptr;};
   script.SubmitTypeLibrary=[](unsigned){};
   script.SetCOMObjectOverride=[](const char* name,const ScriptClassDef*){assert(std::strcmp(name,"ScoreTracker.Recorder")==0);};
   void* library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library){std::cerr<<dlerror()<<'\n';return 1;}
   const auto load=reinterpret_cast<void(*)(uint32_t,const MsgPluginAPI*)>(dlsym(library,"ScoreTrackerPluginLoad"));
   const auto unload=reinterpret_cast<void(*)()>(dlsym(library,"ScoreTrackerPluginUnload"));assert(load&&unload);
   for(unsigned cycle=0;cycle<4;++cycle){
      output=(root/std::to_string(cycle)).string();fs::create_directories(output);tablePath=(fs::path(output)/"AFM.vpx").string();
      notices.clear();logs.clear();sourceLive=true;uiReady=false;notificationsEnabled=cycle!=3;
      values[0]=0;values[1]=1;values[2]=values[3]=0;values[4]=350798730;values[5]=0;values[6]=3;
      load(7,&api);assert(recorder);assert(notices.empty());Event("VPX/OnGameStart:1");
      Frame();assert(notices.empty());uiReady=true;Frame();
      if(notificationsEnabled)assert(notices.size()==1&&notices[0].find("PinMAME capture active")!=std::string::npos);
      else assert(notices.empty());
      for(const auto& msg:notices)assert(msg.find("integration failure")==std::string::npos);
      assert(Games()==0); // Previous game's score and boot values are never appended.
      if(cycle==0){
         Start(2);values[0]=1;Frame();assert(Games()==0); // Earlier-ball game_over pulse.
         values[0]=0;values[3]=2;Frame();values[3]=3;Frame();values[0]=1;Frame();assert(Games()==0); // Last player not observed.
         values[0]=0;values[2]=2;Frame();values[4]=9007199254740993LL;values[5]=0;values[0]=1;Frame();
         assert(Games()==1&&SavedNotices()==1);
         const auto game=History()["games"][0];assert(game["rom"]=="afm_113b");
         assert(!game.contains("evidence"));
         assert(game["scores"][0]["score"]==9007199254740993LL&&game["scores"][1]["score"]==0);
         Frame();Frame();assert(Games()==1&&SavedNotices()==1);
         Start();Complete(0);assert(Games()==2&&SavedNotices()==2);
         Start();sourceLive=false;Event("Controller/" CTLPI_STATE_ON_SRC_CHG_MSG);
         const auto oldReads=reads;Frame();assert(reads==oldReads);
         sourceLive=true;Event("Controller/" CTLPI_STATE_ON_SRC_CHG_MSG);Frame();Complete(555);assert(Games()==2);
         Idle();Start(); // Closing an unfinished game must not write.
      }else if(cycle==1){
         std::ofstream(fs::path(output)/"scores.json")<<"{\"schema_version\":2,\"games\":[]}";
         Idle();Start();Complete(12345);assert(Games()==0&&SavedNotices()==0);
         assert(notices.back().find("failed to save")!=std::string::npos);
         assert(!fs::is_empty(fs::path(output)/".scoretracker-pending"));
         std::ofstream(fs::path(output)/"scores.json")<<"{\"schema_version\":1,\"games\":[]}";
         Event("ScoreTracker/RetryPending:1");assert(Games()==1&&SavedNotices()==1);
         Event("ScoreTracker/RetryPending:1");assert(Games()==1&&SavedNotices()==1);
      }else if(cycle==2){
         const std::string legacy="{\"games\":[{\"old\":true}]}";
         std::ofstream(fs::path(output)/"scores.json")<<legacy;
         Idle();Start();Complete(987);assert(Games()==1&&SavedNotices()==1);
         bool backup=false;for(const auto& file:fs::directory_iterator(output))if(file.path().filename().string().starts_with("scores.legacy-")){
            std::ifstream in(file.path());assert(std::string(std::istreambuf_iterator<char>(in),{})==legacy);backup=true;
         }assert(backup);
      }else{
         Idle();Start();Complete(111);assert(Games()==1&&notices.empty());
         void* object=recorder->CreateObject();ScriptVariant count{};count.vInt=1;assert(Call(object,"BeginGame",&count).vBool);
         ScriptVariant args[2]{};args[0].vInt=1;char score[]="333";args[1].vString.string=score;
         assert(Call(object,"SetPlayerScore",args).vBool);assert(Call(object,"CompleteGame").vBool);Call(object,"Release");assert(Games()==2);
      }
      for(const auto& message:notices)
         assert(std::all_of(message.begin(),message.end(),[](unsigned char c){return c<128;}));
      const auto before=Games(),beforeNotices=notices.size();Event("VPX/OnGameEnd:1");assert(Games()==before&&notices.size()==beforeNotices);
      const auto oldReads=reads;Frame();unload();assert(reads==oldReads&&subscriptions.empty()&&!recorder);
      for(const auto& [id,count]:refs)assert(count==0);
   }
   dlclose(library);fs::remove_all(root);
   std::cout<<"PASS: actual module uses public Controller states only; native completion, player order, exact scores, zero, consecutive games, no exit fallback, source invalidation, failure/retry, legacy backup, script recording and native notifications; no timers, custom PinMAME API or ROM paths\n";
}
