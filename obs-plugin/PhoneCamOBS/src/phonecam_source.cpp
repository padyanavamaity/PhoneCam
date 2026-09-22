#include "phonecam_source.h"
extern "C" const char* phonecam_obs_plugin_name(){ return "PhoneCam OBS"; }
// TODO: integrate OBS SDK, enumerate authenticated local sessions, render video,
// submit per-session audio, and enforce local IPC access control.
