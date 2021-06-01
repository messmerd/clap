#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "plugin.hh"

namespace clap {

   Plugin::Plugin(const clap_plugin_descriptor *desc, const clap_host *host) : host_(host) {
      plugin_.plugin_data      = this;
      plugin_.desc             = desc;
      plugin_.init             = Plugin::clapInit;
      plugin_.destroy          = Plugin::clapDestroy;
      plugin_.extension        = nullptr;
      plugin_.process          = nullptr;
      plugin_.activate         = nullptr;
      plugin_.deactivate       = nullptr;
      plugin_.start_processing = nullptr;
      plugin_.stop_processing  = nullptr;
   }

   /////////////////////
   // CLAP Interfaces //
   /////////////////////

   // clap_plugin interface
   bool Plugin::clapInit(const clap_plugin *plugin) {
      auto &self = from(plugin);

      self.plugin_.extension        = Plugin::clapExtension;
      self.plugin_.process          = Plugin::clapProcess;
      self.plugin_.activate         = Plugin::clapActivate;
      self.plugin_.deactivate       = Plugin::clapDeactivate;
      self.plugin_.start_processing = Plugin::clapStartProcessing;
      self.plugin_.stop_processing  = Plugin::clapStopProcessing;

      self.initInterfaces();
      self.ensureMainThread("clap_plugin.init");
      self.initTrackInfo();
      return self.init();
   }

   void Plugin::clapDestroy(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin.destroy");
      delete &from(plugin);
   }

   bool Plugin::clapActivate(const clap_plugin *plugin, int sample_rate) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin.activate");

      if (self.isActive_) {
         self.hostMisbehaving("Plugin was activated twice");

         if (sample_rate != self.sampleRate_) {
            std::ostringstream msg;
            msg << "The plugin was activated twice and with different sample rates: "
                << self.sampleRate_ << " and " << sample_rate
                << ". The host must deactivate the plugin first." << std::endl
                << "Simulating deactivation.";
            self.hostMisbehaving(msg.str());
            clapDeactivate(plugin);
         }
      }

      if (sample_rate <= 0) {
         std::ostringstream msg;
         msg << "The plugin was activated with an invalid sample rates: " << sample_rate;
         self.hostMisbehaving(msg.str());
         return false;
      }

      assert(!self.isActive_);
      assert(self.sampleRate_ == 0);

      if (!self.activate(sample_rate)) {
         assert(!self.isActive_);
         assert(self.sampleRate_ == 0);
         return false;
      }

      self.isActive_   = true;
      self.sampleRate_ = sample_rate;
      return true;
   }

   void Plugin::clapDeactivate(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin.deactivate");

      if (!self.isActive_) {
         self.hostMisbehaving("The plugin was deactivated twice.");
         return;
      }

      self.deactivate();
   }

   bool Plugin::clapStartProcessing(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureAudioThread("clap_plugin.start_processing");

      if (!self.isActive_) {
         self.hostMisbehaving("Host called clap_plugin.start_processing() on a deactivated plugin");
         return false;
      }

      if (self.isProcessing_) {
         self.hostMisbehaving("Host called clap_plugin.start_processing() twice");
         return true;
      }

      self.isProcessing_ = self.startProcessing();
      return self.isProcessing_;
   }

   void Plugin::clapStopProcessing(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureAudioThread("clap_plugin.stop_processing");

      if (!self.isActive_) {
         self.hostMisbehaving("Host called clap_plugin.stop_processing() on a deactivated plugin");
         return;
      }

      if (!self.isProcessing_) {
         self.hostMisbehaving("Host called clap_plugin.stop_processing() twice");
         return;
      }

      self.stopProcessing();
      self.isProcessing_ = false;
   }

   clap_process_status Plugin::clapProcess(const clap_plugin *plugin, const clap_process *process) {
      auto &self = from(plugin);
      self.ensureAudioThread("clap_plugin.process");

      if (!self.isActive_) {
         self.hostMisbehaving("Host called clap_plugin.process() on a deactivated plugin");
         return CLAP_PROCESS_ERROR;
      }

      if (!self.isProcessing_) {
         self.hostMisbehaving(
            "Host called clap_plugin.process() without calling clap_plugin.start_processing()");
         return CLAP_PROCESS_ERROR;
      }

      return self.process(process);
   }

   const void *Plugin::clapExtension(const clap_plugin *plugin, const char *id) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin.extension");

      if (!strcmp(id, CLAP_EXT_RENDER))
         return &self.pluginRender_;
      if (!strcmp(id, CLAP_EXT_TRACK_INFO))
         return &pluginTrackInfo_;
      if (!strcmp(id, CLAP_EXT_AUDIO_PORTS) && self.implementsAudioPorts())
         return &pluginAudioPorts_;
      if (!strcmp(id, CLAP_EXT_PARAMS) && self.implementsParams())
         return &pluginParams_;

      return from(plugin).extension(id);
   }

   void Plugin::clapTrackInfoChanged(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_track_info.changed");

      if (!self.canUseTrackInfo()) {
         self.hostMisbehaving("Host called clap_plugin_track_info.changed() but does not provide a "
                              "complete clap_host_track_info interface");
         return;
      }

      clap_track_info info;
      if (!self.hostTrackInfo_->get(self.host_, &info)) {
         self.hasTrackInfo_ = false;
         self.hostMisbehaving(
            "clap_host_track_info.get() failed after calling clap_plugin_track_info.changed()");
         return;
      }

      const bool didChannelChange = info.channel_count != self.trackInfo_.channel_count ||
                                    info.channel_map != self.trackInfo_.channel_map;
      self.trackInfo_    = info;
      self.hasTrackInfo_ = true;
      self.trackInfoChanged();
   }

   void Plugin::initTrackInfo() {
      checkMainThread();

      assert(!hasTrackInfo_);
      if (!canUseTrackInfo())
         return;

      hasTrackInfo_ = hostTrackInfo_->get(host_, &trackInfo_);
   }

   uint32_t Plugin::clapAudioPortsCount(const clap_plugin *plugin, bool is_input) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_audio_ports.count");

      return self.audioPortsCount(is_input);
   }

   bool Plugin::clapAudioPortsInfo(const clap_plugin *   plugin,
                                   uint32_t              index,
                                   bool                  is_input,
                                   clap_audio_port_info *info) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_audio_ports.info");
      auto count = clapAudioPortsCount(plugin, is_input);
      if (index >= count) {
         std::ostringstream msg;
         msg << "Host called clap_plugin_audio_ports.info() with an index out of bounds: " << index
             << " >= " << count;
         self.hostMisbehaving(msg.str());
         return false;
      }

      return self.audioPortsInfo(index, is_input, info);
   }

   uint32_t Plugin::clapAudioPortsConfigCount(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_audio_ports.config_count");
      return self.audioPortsConfigCount();
   }

   bool Plugin::clapAudioPortsGetConfig(const clap_plugin *      plugin,
                                        uint32_t                 index,
                                        clap_audio_ports_config *config) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_audio_ports.get_config");

      auto count = clapAudioPortsConfigCount(plugin);
      if (index >= count) {
         std::ostringstream msg;
         msg << "called clap_plugin_audio_ports.get_config with an index out of bounds: " << index
             << " >= " << count;
         self.hostMisbehaving(msg.str());
         return false;
      }
      return self.audioPortsGetConfig(index, config);
   }

   bool Plugin::clapAudioPortsSetConfig(const clap_plugin *plugin, clap_id config_id) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_audio_ports.get_config");

      if (self.isActive())
         self.hostMisbehaving(
            "it is illegal to call clap_audio_ports.set_config if the plugin is active");

      return self.audioPortsSetConfig(config_id);
   }

   uint32_t Plugin::clapParamsCount(const clap_plugin *plugin) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.count");

      return self.paramsCount();
   }

   bool Plugin::clapParamsIinfo(const clap_plugin *plugin,
                                int32_t            param_index,
                                clap_param_info *  param_info) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.info");

      auto count = clapParamsCount(plugin);
      if (param_index >= count) {
         std::ostringstream msg;
         msg << "called clap_plugin_params.info with an index out of bounds: " << param_index
             << " >= " << count;
         self.hostMisbehaving(msg.str());
         return false;
      }

      return self.paramsInfo(param_index, param_info);
   }

   bool Plugin::clapParamsEnumValue(const clap_plugin *plugin,
                                    clap_id            param_id,
                                    int32_t            value_index,
                                    clap_param_value * value) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.enum_value");

      if (!self.isValidParamId(param_id)) {
         std::ostringstream msg;
         msg << "clap_plugin_params.enum_value called with invalid param_id: " << param_id;
         self.hostMisbehaving(msg.str());
         return false;
      }

      // TODO: check the value index?

      return self.paramsEnumValue(param_id, value_index, value);
   }

   bool
   Plugin::clapParamsValue(const clap_plugin *plugin, clap_id param_id, clap_param_value *value) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.value");

      if (!self.isValidParamId(param_id)) {
         std::ostringstream msg;
         msg << "clap_plugin_params.value called with invalid param_id: " << param_id;
         self.hostMisbehaving(msg.str());
         return false;
      }

      // TODO extra checks

      return self.paramsValue(param_id, value);
   }

   bool Plugin::clapParamsSetValue(const clap_plugin *plugin,
                                   clap_id            param_id,
                                   clap_param_value   value,
                                   clap_param_value   modulation) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.set_value");

      if (self.isActive_) {
         self.hostMisbehaving(
            "it is forbidden to call clap_plugin_params.set_value() if the plugin is activated");
         return false;
      }

      if (!self.isValidParamId(param_id)) {
         std::ostringstream msg;
         msg << "clap_plugin_params.set_value called with invalid param_id: " << param_id;
         self.hostMisbehaving(msg.str());
         return false;
      }

      // TODO: extra checks

      return self.paramsSetValue(param_id, value, modulation);
   }

   bool Plugin::clapParamsValueToText(const clap_plugin *plugin,
                                      clap_id            param_id,
                                      clap_param_value   value,
                                      char *             display,
                                      uint32_t           size) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.value_to_text");

      if (!self.isValidParamId(param_id)) {
         std::ostringstream msg;
         msg << "clap_plugin_params.value_to_text called with invalid param_id: " << param_id;
         self.hostMisbehaving(msg.str());
         return false;
      }

      // TODO: extra checks
      return self.paramsValueToText(param_id, value, display, size);
   }

   bool Plugin::clapParamsTextToValue(const clap_plugin *plugin,
                                      clap_id            param_id,
                                      const char *       display,
                                      clap_param_value * value) {
      auto &self = from(plugin);
      self.ensureMainThread("clap_plugin_params.text_to_value");

      if (!self.isValidParamId(param_id)) {
         std::ostringstream msg;
         msg << "clap_plugin_params.text_to_value called with invalid param_id: " << param_id;
         self.hostMisbehaving(msg.str());
         return false;
      }

      // TODO: extra checks
      return self.paramsTextToValue(param_id, display, value);
   }

   bool Plugin::isValidParamId(clap_id param_id) const noexcept {
      checkMainThread();

      auto            count = paramsCount();
      clap_param_info info;
      for (uint32_t i = 0; i < count; ++i) {
         if (!paramsInfo(i, &info))
            // TODO: fatal error?
            continue;

         if (info.id == param_id)
            return true;
      }
      return false;
   }

   /////////////
   // Logging //
   /////////////
   void Plugin::log(clap_log_severity severity, const char *msg) const {
      if (canUseHostLog())
         hostLog_->log(host_, severity, msg);
      else
         std::clog << msg << std::endl;
   }

   void Plugin::hostMisbehaving(const char *msg) { log(CLAP_LOG_HOST_MISBEHAVING, msg); }

   /////////////////////////////////
   // Interface consistency check //
   /////////////////////////////////

   bool Plugin::canUseHostLog() const noexcept { return hostLog_ && hostLog_->log; }

   bool Plugin::canUseThreadCheck() const noexcept {
      return hostThreadCheck_ && hostThreadCheck_->is_audio_thread &&
             hostThreadCheck_->is_main_thread;
   }

   /////////////////////
   // Thread Checking //
   /////////////////////

   void Plugin::checkMainThread() const {
      if (!hostThreadCheck_ || !hostThreadCheck_->is_main_thread ||
          hostThreadCheck_->is_main_thread(host_))
         return;

      std::terminate();
   }

   void Plugin::ensureMainThread(const char *method) {
      if (!hostThreadCheck_ || !hostThreadCheck_->is_main_thread ||
          hostThreadCheck_->is_main_thread(host_))
         return;

      std::ostringstream msg;
      msg << "Host called the method " << method
          << "() on wrong thread! It must be called on main thread!";
      hostMisbehaving(msg.str());
      std::terminate();
   }

   void Plugin::ensureAudioThread(const char *method) {
      if (!hostThreadCheck_ || !hostThreadCheck_->is_audio_thread ||
          hostThreadCheck_->is_audio_thread(host_))
         return;

      std::ostringstream msg;
      msg << "Host called the method " << method
          << "() on wrong thread! It must be called on audio thread!";
      hostMisbehaving(msg.str());
      std::terminate();
   }

   ///////////////
   // Utilities //
   ///////////////
   Plugin &Plugin::from(const clap_plugin *plugin) {
      if (!plugin) {
         std::cerr << "called with a null clap_plugin pointer!" << std::endl;
         std::terminate();
      }

      if (!plugin->plugin_data) {
         std::cerr << "called with a null clap_plugin->plugin_data pointer! The host must never "
                      "change this pointer!"
                   << std::endl;
         std::terminate();
      }

      return *static_cast<Plugin *>(plugin->plugin_data);
   }

   template <typename T>
   void Plugin::initInterface(const T *&ptr, const char *id) {
      assert(!ptr);
      assert(id);

      if (host_->extension)
         ptr = static_cast<const T *>(host_->extension(host_, id));
   }

   void Plugin::initInterfaces() {
      initInterface(hostLog_, CLAP_EXT_LOG);
      initInterface(hostThreadCheck_, CLAP_EXT_THREAD_CHECK);
      initInterface(hostThreadPool_, CLAP_EXT_THREAD_POOL);
      initInterface(hostAudioPorts_, CLAP_EXT_AUDIO_PORTS);
      initInterface(hostEventLoop_, CLAP_EXT_EVENT_LOOP);
      initInterface(hostEventFilter_, CLAP_EXT_EVENT_FILTER);
      initInterface(hostFileReference_, CLAP_EXT_FILE_REFERENCE);
      initInterface(hostLatency_, CLAP_EXT_LATENCY);
      initInterface(hostGui_, CLAP_EXT_GUI);
      initInterface(hostParams_, CLAP_EXT_PARAMS);
      initInterface(hostTrackInfo_, CLAP_EXT_TRACK_INFO);
      initInterface(hostState_, CLAP_EXT_STATE);
      initInterface(hostNoteName_, CLAP_EXT_NOTE_NAME);
   }

   uint32_t Plugin::compareAudioPortsInfo(const clap_audio_port_info &a,
                                          const clap_audio_port_info &b) noexcept {
      if (a.sample_size != b.sample_size || a.in_place != b.in_place || a.is_cv != b.is_cv ||
          a.is_main != b.is_main || a.channel_count != b.channel_count ||
          a.channel_map != b.channel_map || a.id != b.id)
         return CLAP_AUDIO_PORTS_RESCAN_ALL;

      if (strncmp(a.name, b.name, sizeof(a.name)))
         return CLAP_AUDIO_PORTS_RESCAN_NAMES;

      return 0;
   }

   int Plugin::sampleRate() const noexcept {
      assert(isActive_ &&
             "there is no point in querying the sample rate if the plugin isn't activated");
      assert(isActive_ ? sampleRate_ > 0 : sampleRate_ == 0);
      return sampleRate_;
   }
} // namespace clap