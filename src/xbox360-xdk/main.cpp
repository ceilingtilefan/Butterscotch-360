// Butterscotch Xbox 360 — XDK Entry Point
// Uses official Xbox 360 SDK: D3D9, XAudio2, XInputGetState

#include <xtl.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// DbgPrint is a C-linkage kernel function — declare it explicitly since
// we compile .c files as C++ and removed extern "C" wrappers.
extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "data_win.h"
#include "json_reader.h"
#include "utils.h"
#include "stb_ds.h"

#include "d3d9_renderer.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"

// ===[ POSIX clock polyfill implementation ]===
double _xdk_monotonic_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// Screen dimensions (720p native)
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

// ===[ Controller Mapping ]===

typedef struct {
    WORD xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = NULL;
static int xpadMappingCount = 0;
static WORD prevButtons = 0;
static BYTE prevLeftTrigger = 0;
static BYTE prevRightTrigger = 0;

static void setupDefaultMappings(void) {
    static XpadMapping defaults[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    38 },  // VK_UP
        { XINPUT_GAMEPAD_DPAD_DOWN,  40 },  // VK_DOWN
        { XINPUT_GAMEPAD_DPAD_LEFT,  37 },  // VK_LEFT
        { XINPUT_GAMEPAD_DPAD_RIGHT, 39 },  // VK_RIGHT
        { XINPUT_GAMEPAD_A,          13 },  // VK_RETURN (confirm)
        { XINPUT_GAMEPAD_B,          16 },  // VK_SHIFT (cancel)
        { XINPUT_GAMEPAD_X,          17 },  // VK_CONTROL
        { XINPUT_GAMEPAD_Y,          88 },  // 'X' key
        { XINPUT_GAMEPAD_START,      27 },  // VK_ESCAPE (menu)
        { XINPUT_GAMEPAD_BACK,       27 },  // VK_ESCAPE
    };
    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
    xpadMappings = (XpadMapping*)malloc(sizeof(defaults));
    memcpy(xpadMappings, defaults, sizeof(defaults));
}

// ===[ Main Entry Point ]===

VOID __cdecl main() {
    DbgPrint("BS: main() entered\n");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { DbgPrint("BS: FATAL: D3D create failed\n"); return; }
    DbgPrint("BS: D3D9 created\n");

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth        = SCREEN_WIDTH;
    d3dpp.BackBufferHeight       = SCREEN_HEIGHT;
    d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    d3dpp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    d3dpp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* pd3dDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &pd3dDevice);
    if (FAILED(hr)) {
        DbgPrint("BS: FATAL: CreateDevice failed\n");
        return;
    }
    DbgPrint("BS: D3D device created\n");


    // ===[ Locate data.win ]===
    // On Xbox 360, game content is at game:\ (DVD/HDD) or d:\ (dev kit)
    const char* dataWinPath = NULL;

    // Try multiple paths. Use fopen() for detection since it goes through
    // the Xbox CRT which handles game:\ paths reliably on both hardware and emulators.
    static const char* searchPaths[] = {
        "game:\\data.win",
        "game:\\butterscotch\\data.win",
        "d:\\data.win",
        "d:\\butterscotch\\data.win",
        NULL,
    };

    DbgPrint("BS: searching for data.win\n");
    char msg[256];
    for (int i = 0; searchPaths[i]; i++) {
        DbgPrint("BS: trying %s\n", searchPaths[i]);

        FILE* testFile = fopen(searchPaths[i], "rb");
        if (testFile) {
            fclose(testFile);
            dataWinPath = searchPaths[i];
            DbgPrint("BS: found data.win at %s\n", dataWinPath);
            break;
        }
    }

    if (!dataWinPath) {
        DbgPrint("BS: FATAL: data.win not found\n");
        for (;;) { } // hang instead of crash for debugging
    }

    DbgPrint("BS: parsing data.win\n");

    DataWinParserOptions parseOpts;
    memset(&parseOpts, 0, sizeof(parseOpts));
    parseOpts.parseGen8 = true;  parseOpts.parseOptn = true;  parseOpts.parseLang = true;
    parseOpts.parseExtn = true;  parseOpts.parseSond = true;  parseOpts.parseAgrp = true;
    parseOpts.parseSprt = true;  parseOpts.parseBgnd = true;  parseOpts.parsePath = true;
    parseOpts.parseScpt = true;  parseOpts.parseGlob = true;  parseOpts.parseShdr = true;
    parseOpts.parseFont = true;  parseOpts.parseTmln = true;  parseOpts.parseObjt = true;
    parseOpts.parseRoom = true;  parseOpts.parseTpag = true;  parseOpts.parseCode = true;
    parseOpts.parseVari = true;  parseOpts.parseFunc = true;  parseOpts.parseStrg = true;
    parseOpts.parseTxtr = true;  parseOpts.parseAudo = true;
    parseOpts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    DataWin* dataWin = DataWin_parse(dataWinPath, parseOpts);

    if (!dataWin) {
        DbgPrint("BS: FATAL: DataWin_parse returned NULL\n");
        for (;;) { }
    }
    DbgPrint("BS: data.win parsed OK\n");

    DbgPrint("BS: game=%s\n", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown");

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        sprintf(configPath + dirLen, "CONFIG.JSN");
    } else {
        strcpy(configPath, "CONFIG.JSN");
    }

    JsonValue* configRoot = NULL;
    HANDLE hConfig = CreateFileA(configPath, GENERIC_READ, FILE_SHARE_READ,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hConfig != INVALID_HANDLE_VALUE) {
        DWORD configSize = GetFileSize(hConfig, NULL);
        char* configText = (char*)malloc(configSize + 1);
        DWORD bytesRead;
        ReadFile(hConfig, configText, configSize, &bytesRead, NULL);
        CloseHandle(hConfig);
        configText[bytesRead] = '\0';
        configRoot = JsonReader_parse(configText);
        free(configText);
        OutputDebugStringA("Loaded CONFIG.JSN\n");
    }

    // ===[ Create Subsystems ]===
    DbgPrint("BS: creating subsystems\n");
    XdkFileSystem* xdkFs = XdkFileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*)xdkFs;

    DbgPrint("BS: creating renderer\n");
    Renderer* renderer = D3D9Renderer_create(pd3dDevice);

    DbgPrint("BS: creating VM\n");
    VMContext* vm = VM_create(dataWin);
    DbgPrint("BS: creating runner\n");
    Runner* runner = Runner_create(dataWin, vm, fileSystem);

    // Parse CONFIG.JSN options
    if (configRoot) {
        JsonValue* disabledArr = JsonReader_getObject(configRoot, "disabledObjects");
        if (disabledArr && JsonReader_isArray(disabledArr)) {
            sh_new_strdup(runner->disabledObjects);
            int count = JsonReader_arrayLength(disabledArr);
            for (int i = 0; i < count; i++) {
                JsonValue* elem = JsonReader_getArrayElement(disabledArr, i);
                if (elem && JsonReader_isString(elem)) {
                    const char* name = JsonReader_getString(elem);
                    shput(runner->disabledObjects, name, 1);
                }
            }
        }

        JsonValue* mappingsObj = JsonReader_getObject(configRoot, "controllerMappings");
        if (mappingsObj && JsonReader_isObject(mappingsObj)) {
            xpadMappingCount = JsonReader_objectLength(mappingsObj);
            xpadMappings = (XpadMapping*)malloc(sizeof(XpadMapping) * xpadMappingCount);
            for (int i = 0; i < xpadMappingCount; i++) {
                const char* btnStr = JsonReader_getObjectKey(mappingsObj, i);
                JsonValue* gmlVal = JsonReader_getObjectValue(mappingsObj, i);
                xpadMappings[i].xpadButton = (WORD)atoi(btnStr);
                xpadMappings[i].gmlKey = (int32_t)JsonReader_getInt(gmlVal);
            }
        }
    }

    if (!xpadMappings) setupDefaultMappings();

    runner->renderer = renderer;

    // Initialize audio
    DbgPrint("BS: creating audio\n");
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)xdkAudio;
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);
    runner->audioSystem = audioSystem;
    DbgPrint("BS: audio OK\n");

    // Initialize renderer
    DbgPrint("BS: init renderer\n");
    renderer->vtable->init(renderer, dataWin);
    DbgPrint("BS: renderer OK\n");

    // Initialize first room
    DbgPrint("BS: init first room\n");
    Runner_initFirstRoom(runner);
    DbgPrint("BS: first room OK\n");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    DbgPrint("BS: gameW=%d gameH=%d screenW=%d screenH=%d\n", gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Parse deferDrawToAfterAllSteps
    bool deferDraw = false;
    if (configRoot) {
        JsonValue* deferVal = JsonReader_getObject(configRoot, "deferDrawToAfterAllSteps");
        if (deferVal) deferDraw = JsonReader_getBool(deferVal);
    }

    DbgPrint("BS: entering main loop\n");

    // ===[ Main Loop ]===
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    double accumulator = 0.0;

    while (!runner->shouldExit) {
        QueryPerformanceCounter(&currentTime);
        double deltaTime = (double)(currentTime.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
        lastTime = currentTime;

        // ===[ Poll Controller ]===
        XINPUT_STATE state;
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            WORD buttons = state.Gamepad.wButtons;

            // Left thumbstick as dpad
            #define STICK_DEADZONE 16384
            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            if (lx < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (lx >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (ly >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;

            for (int i = 0; i < xpadMappingCount; i++) {
                WORD mask = xpadMappings[i].xpadButton;
                int32_t gmlKey = xpadMappings[i].gmlKey;

                bool wasPressed = (prevButtons & mask) != 0;
                bool isPressed = (buttons & mask) != 0;

                if (isPressed && !wasPressed)
                    RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                else if (!isPressed && wasPressed)
                    RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
            }
            prevButtons = buttons;
            prevRightTrigger = state.Gamepad.bRightTrigger;
        }

        bool speedCapRemoved = prevRightTrigger > 128;

        // ===[ Frame Pacing ]===
        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / roomSpeed) : (1.0 / 60.0);

        accumulator += deltaTime;
        double maxAccumulator = targetFrameTime * 4.0;
        if (accumulator > maxAccumulator) accumulator = maxAccumulator;
        if (speedCapRemoved && targetFrameTime > accumulator) accumulator = targetFrameTime;

        int gameFramesRan = 0;
        while (accumulator >= targetFrameTime) {
            if (gameFramesRan > 0)
                RunnerKeyboard_beginFrame(runner->keyboard);

            Runner_step(runner);

            if (!deferDraw) {
                renderer->vtable->beginFrame(renderer, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

                Room* activeRoom = runner->currentRoom;
                bool anyViewRendered = false;
                bool viewsEnabled = (activeRoom->flags & 1) != 0;

                if (viewsEnabled) {
                    for (int vi = 0; vi < 8; vi++) {
                        if (!activeRoom->views[vi].enabled) continue;
                        runner->viewCurrent = vi;
                        renderer->vtable->beginView(renderer,
                            activeRoom->views[vi].viewX, activeRoom->views[vi].viewY,
                            activeRoom->views[vi].viewWidth, activeRoom->views[vi].viewHeight,
                            activeRoom->views[vi].portX, activeRoom->views[vi].portY,
                            activeRoom->views[vi].portWidth, activeRoom->views[vi].portHeight,
                            runner->viewAngles[vi]);
                        Runner_draw(runner);
                        renderer->vtable->endView(renderer);
                        anyViewRendered = true;
                    }
                }

                if (!anyViewRendered) {
                    runner->viewCurrent = 0;
                    renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
                    Runner_draw(runner);
                    renderer->vtable->endView(renderer);
                }

                runner->viewCurrent = 0;
                renderer->vtable->endFrame(renderer);
            }

            accumulator -= targetFrameTime;
            gameFramesRan++;
        }

        // Deferred draw: render once after all catch-up steps
        if (deferDraw && gameFramesRan > 0) {
            renderer->vtable->beginFrame(renderer, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

            Room* activeRoom = runner->currentRoom;
            bool anyViewRendered = false;
            bool viewsEnabled = (activeRoom->flags & 1) != 0;

            if (viewsEnabled) {
                for (int vi = 0; vi < 8; vi++) {
                    if (!activeRoom->views[vi].enabled) continue;
                    runner->viewCurrent = vi;
                    renderer->vtable->beginView(renderer,
                        activeRoom->views[vi].viewX, activeRoom->views[vi].viewY,
                        activeRoom->views[vi].viewWidth, activeRoom->views[vi].viewHeight,
                        activeRoom->views[vi].portX, activeRoom->views[vi].portY,
                        activeRoom->views[vi].portWidth, activeRoom->views[vi].portHeight,
                        runner->viewAngles[vi]);
                    Runner_draw(runner);
                    renderer->vtable->endView(renderer);
                    anyViewRendered = true;
                }
            }

            if (!anyViewRendered) {
                runner->viewCurrent = 0;
                renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
                Runner_draw(runner);
                renderer->vtable->endView(renderer);
            }

            runner->viewCurrent = 0;
            renderer->vtable->endFrame(renderer);
        }

        // Update audio
        if (runner->audioSystem) {
            float dt = (float)deltaTime;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }

        if (gameFramesRan > 0) {
            RunnerKeyboard_beginFrame(runner->keyboard);
        }
    }

    // ===[ Cleanup ]===
    if (runner->audioSystem) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = NULL;
    }
    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);
    pd3dDevice->Release();
    pD3D->Release();

    free(xpadMappings);
}
