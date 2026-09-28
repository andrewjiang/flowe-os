"""Host tests compile the real SDK facade and Gfx; no display/model reimplementation."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[4]
OS = ROOT / "firmware/xphone-os"
SDK = ROOT / "firmware/freeink-sdk/libs/display/FreeInkDisplay"
STUBS = OS / "test/host/sync_stubs"

def method(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth, end = 1, body + 1
    while depth:
        if source[end] == "{": depth += 1
        elif source[end] == "}": depth -= 1
        end += 1
    return source[start:end]

def load(name):
    spec = importlib.util.spec_from_file_location(name, OS / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class SyncMemoryTest(unittest.TestCase):
    def compile_run(self, sources, flags=()):
        if OS / "src/Gfx.cpp" in sources:
            sources = [*sources, OS / "lib/Utf8/Utf8.cpp"]
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "test"
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined", "-g",
                            "-I" + str(STUBS), "-I" + str(SDK / "include"),
                            "-I" + str(OS / "lib/EpdFontCore"),
                            "-I" + str(OS / "lib/Utf8"), *flags,
                            *map(str, sources), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_scene_lifecycle(self):
        source = (OS / "src/scenes/FileTransferScene.cpp").read_text()
        methods = []
        for name in ["activateSyncMemory", "restoreSyncMemory", "endSession", "onExit", "silentTick"]:
            result_type = "bool" if name == "activateSyncMemory" else "void"
            methods.append(method(source, result_type + " FileTransferScene::" + name + "("))
        harness = (OS / "test/host/sync_scene_harness.cpp").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / "scene.cpp"
            test.write_text(harness.replace("// PRODUCTION_METHODS", "\n".join(methods)))
            self.compile_run([test])

    def test_status_bar_preserves_page_pixels(self):
        source = (OS / "src/scenes/FileTransferScene.cpp").read_text()
        draw = method(source, "bool FileTransferScene::paintPill(")
        # The real fonts and Gfx draw the bar into both actual panel geometries.
        # Only the 44-pixel band at the physical buttons may change.
        scene = (OS / "src/Scene.h").read_text()
        bar_size = scene[scene.index("static constexpr int SOFTKEY_BAR_H") :].split(";", 1)[0] + ";"
        harness = r'''
        #include "Gfx.h"
        #include "Fonts.h"
        #include <cassert>
        #include <cstdio>
        #include <cstring>
        #include <vector>
        #include <iostream>
        Gfx* G_GFX=nullptr;
        struct Scene { BAR_SIZE };
        struct {bool busy=false;bool flushInFlight(){return busy;}} SCENES;
        struct FileTransferScene {
          bool _staticSync=false,_directMode=false;
          char _ssid[64]="2521Midvale",_pillShown[40]={};
          bool paintPill(const char*);
        };
        PRODUCTION_DRAW
        int main(){
          for(bool landscape:{false,true})for(bool direct:{false,true}){
            EInkDisplay display;Gfx gfx(display);assert(gfx.begin());G_GFX=&gfx;
            gfx.setOrientation(landscape?Gfx::Orient::Landscape:Gfx::Orient::Portrait);
            for(size_t i=0;i<sizeof(display.bytes);++i)display.bytes[i]=(i*73+19)%256;
            std::vector<uint8_t> before(display.bytes,display.bytes+sizeof(display.bytes));
            FileTransferScene s;s._directMode=direct;assert(s.paintPill("Syncing..."));
            int changed=0,black=0,white=0;
            for(int y=0;y<display.getDisplayHeight();++y)for(int x=0;x<display.getDisplayWidth();++x){
              const int index=y*display.getDisplayWidthBytes()+x/8,mask=0x80>>(x%8);
              if(x<display.getDisplayWidth()-Scene::SOFTKEY_BAR_H)
                assert((display.bytes[index]&mask)==(before[index]&mask));
              else {
                changed+=(display.bytes[index]&mask)!=(before[index]&mask);
                if(display.bytes[index]&mask)++white;else ++black;
              }
            }
            assert(changed>0 && black>white && white>0);
            before.assign(display.bytes,display.bytes+sizeof(display.bytes));
            SCENES.busy=true;assert(!s.paintPill("Waiting for your phone..."));SCENES.busy=false;
            s._staticSync=true;assert(!s.paintPill("Waiting for your phone..."));s._staticSync=false;
            assert(gfx.releaseFramebufferForSync());assert(!s.paintPill("Waiting for your phone..."));
            assert(memcmp(display.bytes,before.data(),before.size())==0);
          }
          std::cout<<"PASS: real status bar changes only the button band; portrait/landscape, shared/hotspot, frozen guards\n";
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / "bar.cpp"
            test.write_text(harness.replace("BAR_SIZE", bar_size).replace("PRODUCTION_DRAW", draw))
            for flags in [[], ["-DFLOWE_TEST_DISPLAY_X3=1"]]:
                self.compile_run([test, OS / "src/Gfx.cpp", OS / "src/Fonts.cpp"],
                                 ["-I" + str(OS / "src"), *flags])

    def test_phone_start_preserves_orientation(self):
        source = (OS / "src/scenes/AppScenes.cpp").read_text()
        start = method(source, "void showFileTransferAutoStartInPlace(")
        harness = r'''
        #include <cassert>
        #include <cstdint>
        enum class SceneId:uint32_t {Launcher,Reader,FileTransfer};
        SceneId gCurrentSceneId=SceneId::Reader;
        struct Gfx {
          enum class Orient{Portrait,Landscape};Orient current=Orient::Landscape;
          Orient orientation(){return current;}void setOrientation(Orient value){current=value;}
        } gfx;
        Gfx* G_GFX=&gfx;
        Gfx::Orient expected=Gfx::Orient::Landscape;
        struct {
          bool direct=false;uint32_t returnScene=0;
          void beginSilent(uint32_t from){returnScene=from;assert(gfx.orientation()==expected);}
          void autoStart(){direct=false;}void autoStartDirect(){direct=true;}
        } gFileTransfer;
        struct {
          bool composed=false;
          void composeActive(Gfx&){composed=true;}
          template<class T>void switchTo(T&){assert(composed);gfx.setOrientation(Gfx::Orient::Portrait);}
        } SCENES;
        PRODUCTION_START
        int main(){
          for(auto orientation:{Gfx::Orient::Portrait,Gfx::Orient::Landscape})for(bool direct:{false,true}){
            gCurrentSceneId=SceneId::Reader;gfx.setOrientation(orientation);expected=orientation;SCENES.composed=false;
            showFileTransferAutoStartInPlace(direct);
            assert(gfx.orientation()==orientation && gFileTransfer.direct==direct);
            assert(gFileTransfer.returnScene==static_cast<uint32_t>(SceneId::Reader));
            assert(gCurrentSceneId==SceneId::FileTransfer);
          }
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / "start.cpp"
            test.write_text("#include <initializer_list>\n" + harness.replace("PRODUCTION_START", start))
            self.compile_run([test])

    def test_status_readiness_barrier(self):
        source = (OS / "src/net/FileTransferServer.cpp").read_text()
        start = source.index("void FileTransferServer::handleStatus()")
        end = source.index("\nvoid FileTransferServer::handleFileList()", start)
        harness = (OS / "test/host/sync_ready_harness.cpp").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / "status.cpp"
            test.write_text(harness.replace("// PRODUCTION_STATUS", source[start:end]))
            for flags in [["-DFLOWE_RAW_UPLOAD=1"], ["-DFLOWE_RAW_UPLOAD=1", "-DFLOWE_SYNC_FAST_SDK=1"]]:
                self.compile_run([test], flags)

    def test_hotspot_reconnect_timer(self):
        source = (OS / "src/scenes/FileTransferScene.cpp").read_text()
        start = source.index("          const int clients = WiFi.softAPgetStationNum();")
        end = source.index("          // A phone that joined and then went quiet", start)
        harness = r'''
        #include <cassert>
        #include <cstdint>
        struct { int clients=0; int softAPgetStationNum(){return clients;} } WiFi;
        struct { template<class... T> void printf(T...){} void println(const char*){} } Serial;
        struct Scene {
          int _shownApClients=0; uint32_t _apClientLeftMs=0, _apStartedMs=1;
          bool _staticSync=false, ended=false;
          static constexpr uint32_t kApNoClientTimeoutMs=120000;
          void markDirty(){} void endSession(const char*){ended=true;}
          void poll(uint32_t now) { PRODUCTION_POLL }
        };
        int main(){
          Scene s; s._staticSync=true;
          s.poll(119000); assert(s._apClientLeftMs==119000 && !s.ended);
          s.poll(121000); assert(!s.ended); // missed join still gets full grace
          WiFi.clients=1; s.poll(130000); assert(!s.ended && s._apClientLeftMs==0);
          WiFi.clients=0; s.poll(140000); assert(s._apClientLeftMs==140000 && !s.ended);
          s.poll(260000); assert(!s.ended); s.poll(260001); assert(s.ended);
          Scene never; never.poll(120001); assert(!never.ended);
          never.poll(120002); assert(never.ended);
          Scene late; late._staticSync=true; late._shownApClients=-1;
          late.poll(125000); assert(!late.ended && late._apClientLeftMs==125000);
          late.poll(245001); assert(late.ended);
        }
        '''
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / "timer.cpp"
            test.write_text(harness.replace("PRODUCTION_POLL", source[start:end]))
            self.compile_run([test])

    def test_sdk_ownership_and_allocation_policies(self):
        for flags in [("-DFREEINK_FB_RELEASABLE=1", "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1"),
                      ("-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1",), (),
                      ("-DFREEINK_FB_PSRAM=1", "-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1"),
                      ("-DFREEINK_FB_PSRAM=1",)]:
            with self.subTest(flags=flags):
                self.compile_run([SDK / "src/FreeInkDisplay.cpp", OS / "test/host/sync_display_test.cpp"],
                                 ["-DARDUINO=1", *flags])

    def test_disabled_gfx(self):
        self.compile_run([OS / "src/Gfx.cpp", OS / "test/host/sync_gfx_test.cpp"])

    def test_raw_control_poll(self):
        self.compile_run([OS / "test/host/sync_raw_cancel_test.cpp"], ["-DFLOWE_SYNC_CONTROL=1"])

    def test_download_control_poll(self):
        self.compile_run([OS / "test/host/sync_write_test.cpp"])

    def test_multipart_patch(self):
        module = load("patch_webserver_sync")
        once = module.patched_text("before\n" + module.OLD + "\nafter\n")
        self.assertEqual(once, module.patched_text(once))
        with self.assertRaises(RuntimeError): module.patched_text("changed source")
        with self.assertRaises(RuntimeError): module.patched_text(once.replace(module.DECL, ""))

    def test_fast_sdk_rejects_wrong_provenance(self):
        module = load("verify_sync_sdk")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError): module.verify(tmp)

if __name__ == "__main__": unittest.main()
