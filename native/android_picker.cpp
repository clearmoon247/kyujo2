#include <SDL.h>
#include <jni.h>
#include <mutex>
#include <string>
#include <cstdio>
namespace {
std::mutex lock;
struct Selection { int port,kind;std::string path,msg;bool ready; } selection{};
std::string string(JNIEnv *env,jstring s) {
    if(!s)return {};
    auto p=env->GetStringUTFChars(s,nullptr); if(!p)return {};
    std::string result=p;env->ReleaseStringUTFChars(s,p);return result;
}
}
extern "C" int host_pick_gb_start(int port,int kind,const char *rom) {
    auto env=static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if(!env || !activity)return 0;
    auto clazz=env->GetObjectClass(activity);
    auto method=env->GetMethodID(clazz,"pickGb","(IILjava/lang/String;)V");
    auto path=env->NewStringUTF(rom?rom:"");
    if(method) env->CallVoidMethod(activity,method,port,kind,path);
    bool ok=method && !env->ExceptionCheck(); if(!ok)env->ExceptionClear();
    env->DeleteLocalRef(path);env->DeleteLocalRef(clazz);env->DeleteLocalRef(activity);
    return ok;
}
extern "C" JNIEXPORT void JNICALL Java_org_kyujo2_game_Game_nativeGbPicked(JNIEnv *env,jclass,jint port,jint kind,jstring path,jstring brief,jstring) {
    std::lock_guard guard(lock);selection={port,kind,string(env,path),string(env,brief),true};
}
extern "C" int host_pick_gb_poll(int *port,int *kind,char *path,unsigned plen,char *msg,unsigned mlen) {
    std::lock_guard guard(lock);if(!selection.ready)return 0;
    *port=selection.port;*kind=selection.kind;std::snprintf(path,plen,"%s",selection.path.c_str());std::snprintf(msg,mlen,"%s",selection.msg.c_str());
    selection.ready=false;return 1;
}
void android_restart_game() {
    auto env=static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());auto activity=static_cast<jobject>(SDL_AndroidGetActivity());
    if(!env||!activity)return;
    auto cls=env->GetObjectClass(activity);auto m=env->GetMethodID(cls,"restartGame","()V");
    if(m)env->CallVoidMethod(activity,m);env->DeleteLocalRef(cls);env->DeleteLocalRef(activity);
}
