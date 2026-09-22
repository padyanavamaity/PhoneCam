#include <iostream>
#include "phonecam/SessionManager.h"
int main(){ phonecam::SessionManager sm; sm.addSession({"demo-camera-1","demo-session-1",true,true,false,1.0f,0}); std::cout<<"PhoneCam Desktop core initialized.
"; std::cout<<"Next layer: native WebRTC receiver + decoder + UI + IPC.
"; }
