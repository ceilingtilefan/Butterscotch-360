#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xenos/xenos.h>
#include <xenos/xe.h>
#include <input/input.h>
#include <ppc/timebase.h>
#include <diskio/ata.h>
#include <usb/usbmain.h>
#include <console/console.h>

#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "../data_win.h"
#include "../json_reader.h"
#include "xbox360_file_system.h"
#include "xbox360_audio_system.h"
#include "xenos_renderer.h"
#include "utils.h"

#include "stb_ds.h"

// Xbox 360 screen dimensions (720p)
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

// Xbox 360 has 512 MB unified RAM
static int MAX_MEMORY_BYTES = 536870912;

// Controller button to GML key mapping
typedef struct {
    uint32_t xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = nullptr;
static int xpadMappingCount = 0;

// Previous frame's button state for detecting press/release edges
static uint32_t prevButtons = 0;

// Pack libxenon's individual-field controller struct into a bitmask
static uint32_t packControllerButtons(const struct controller_data_s* c) {
    uint32_t b = 0;
    if (c->up)    b |= 0x0001;
    if (c->down)  b |= 0x0002;
    if (c->left)  b |= 0x0004;
    if (c->right) b |= 0x0008;
    if (c->start) b |= 0x0010;
    if (c->back)  b |= 0x0020;
    if (c->lb)    b |= 0x0100;
    if (c->rb)    b |= 0x0200;
    if (c->a)     b |= 0x1000;
    if (c->b)     b |= 0x2000;
    if (c->x)     b |= 0x4000;
    if (c->y)     b |= 0x8000;
    return b;
}

// Default Xbox 360 controller -> GML key mappings
// These match the standard Undertale controls
static void setupDefaultMappings(void) {
    // libxenon controller_data_s has individual int fields, not a bitmask.
    // We pack them into a bitmask for uniform mapping.
    // Bit assignments (our convention):
    //   0x0001 = up,  0x0002 = down, 0x0004 = left, 0x0008 = right
    //   0x0010 = start, 0x0020 = back
    //   0x1000 = a, 0x2000 = b, 0x4000 = x, 0x8000 = y
    //   0x0100 = lb, 0x0200 = rb

    static XpadMapping defaults[] = {
        { 0x0001, 38 },   // D-Pad Up    -> VK_UP
        { 0x0002, 40 },   // D-Pad Down  -> VK_DOWN
        { 0x0004, 37 },   // D-Pad Left  -> VK_LEFT
        { 0x0008, 39 },   // D-Pad Right -> VK_RIGHT
        { 0x1000, 13 },   // A           -> VK_RETURN (confirm)
        { 0x2000, 16 },   // B           -> VK_SHIFT (cancel)
        { 0x4000, 17 },   // X           -> VK_CONTROL
        { 0x8000, 88 },   // Y           -> 'X' key
        { 0x0010, 27 },   // Start       -> VK_ESCAPE (menu)
        { 0x0020, 27 },   // Back        -> VK_ESCAPE
    };

    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
    xpadMappings = safeMalloc(sizeof(defaults));
    memcpy(xpadMappings, defaults, sizeof(defaults));
}

int main(void) {
    // ===[ Initialize Xbox 360 Hardware ]===

    // Initialize Xenos GPU
    struct XenosDevice _xe;
    struct XenosDevice* xe = &_xe;
    Xe_Init(xe);
    Xe_SetRenderTarget(xe, Xe_GetFramebufferSurface(xe));

    // Initialize USB (for controllers and USB storage)
    usb_init();
    usb_do_poll();

    // Initialize disk I/O (for HDD access)
    xenon_ata_init();

    printf("Butterscotch Xbox 360 - Starting\n");

    // ===[ Locate data.win ]===
    // Try USB first (uda:/), then HDD (hdd:/)
    const char* dataWinPath = nullptr;
    FILE* test;

    test = fopen("uda:/butterscotch/data.win", "rb");
    if (test != nullptr) {
        fclose(test);
        dataWinPath = "uda:/butterscotch/data.win";
    }

    if (dataWinPath == nullptr) {
        test = fopen("uda:/data.win", "rb");
        if (test != nullptr) {
            fclose(test);
            dataWinPath = "uda:/data.win";
        }
    }

    if (dataWinPath == nullptr) {
        printf("ERROR: Could not find data.win on USB (uda:/butterscotch/data.win or uda:/data.win)\n");
        printf("Please place your game files on a USB drive.\n");
        while (1) { usb_do_poll(); }
    }

    printf("Found data.win at %s\n", dataWinPath);

    // ===[ Parse data.win ]===
    printf("Loading data.win...\n");

    DataWin* dataWin = DataWin_parse(
        dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true, // Xbox 360 has plenty of RAM, load TXTR PNGs
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = nullptr,
            .progressCallbackUserData = nullptr,
        }
    );

    printf("Loaded: %s\n", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown Game");

    // ===[ Create Renderer ]===
    printf("Creating renderer...\n");
    Renderer* renderer = XenosRenderer_create(xe);

    // ===[ Create VM and Runner ]===
    printf("Creating VM and runner...\n");

    // ===[ Load CONFIG.JSN (optional) ]===
    // Build path relative to data.win location
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash != nullptr) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        snprintf(configPath + dirLen, sizeof(configPath) - dirLen, "CONFIG.JSN");
    } else {
        snprintf(configPath, sizeof(configPath), "CONFIG.JSN");
    }

    JsonValue* configRoot = nullptr;
    FILE* configFile = fopen(configPath, "rb");
    if (configFile != nullptr) {
        fseek(configFile, 0, SEEK_END);
        long configSize = ftell(configFile);
        fseek(configFile, 0, SEEK_SET);

        char* configJsonText = safeMalloc((size_t) configSize + 1);
        size_t configBytesRead = fread(configJsonText, 1, (size_t) configSize, configFile);
        configJsonText[configBytesRead] = '\0';
        fclose(configFile);

        configRoot = JsonReader_parse(configJsonText);
        free(configJsonText);
        printf("Loaded CONFIG.JSN\n");
    }

    // Create file system
    X360FileSystem* x360fs = X360FileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*) x360fs;

    VMContext* vm = VM_create(dataWin);
    Runner* runner = Runner_create(dataWin, vm, fileSystem);

    // Parse CONFIG.JSN options
    if (configRoot != nullptr) {
        // Parse disabledObjects
        JsonValue* disabledObjectsArr = JsonReader_getObject(configRoot, "disabledObjects");
        if (disabledObjectsArr != nullptr && JsonReader_isArray(disabledObjectsArr)) {
            sh_new_strdup(runner->disabledObjects);
            int disabledCount = JsonReader_arrayLength(disabledObjectsArr);
            repeat(disabledCount, i) {
                JsonValue* elem = JsonReader_getArrayElement(disabledObjectsArr, i);
                if (elem != nullptr && JsonReader_isString(elem)) {
                    const char* objName = JsonReader_getString(elem);
                    shput(runner->disabledObjects, objName, 1);
                    printf("Disabled object: %s\n", objName);
                }
            }
        }

        // Parse controllerMappings (override defaults)
        JsonValue* controllerMappingsObj = JsonReader_getObject(configRoot, "controllerMappings");
        if (controllerMappingsObj != nullptr && JsonReader_isObject(controllerMappingsObj)) {
            xpadMappingCount = JsonReader_objectLength(controllerMappingsObj);
            xpadMappings = safeMalloc(sizeof(XpadMapping) * xpadMappingCount);
            repeat(xpadMappingCount, i) {
                const char* buttonStr = JsonReader_getObjectKey(controllerMappingsObj, i);
                JsonValue* gmlKeyVal = JsonReader_getObjectValue(controllerMappingsObj, i);
                xpadMappings[i].xpadButton = (uint32_t) atoi(buttonStr);
                xpadMappings[i].gmlKey = (int32_t) JsonReader_getInt(gmlKeyVal);
                printf("CONFIG.JSN: controllerMapping button=%u -> gmlKey=%d\n", xpadMappings[i].xpadButton, xpadMappings[i].gmlKey);
            }
        }
    }

    // Use default mappings if CONFIG.JSN didn't provide any
    if (xpadMappings == nullptr) {
        setupDefaultMappings();
    }

    runner->renderer = renderer;

    // ===[ Initialize Audio System ]===
    printf("Initializing audio...\n");
    X360AudioSystem* x360Audio = X360AudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*) x360Audio;
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);
    runner->audioSystem = audioSystem;

    // ===[ Initialize Renderer ]===
    printf("Initializing renderer...\n");
    renderer->vtable->init(renderer, dataWin);

    // ===[ Initialize First Room ]===
    printf("Initializing first room...\n");
    Runner_initFirstRoom(runner);

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;

    printf("Game resolution: %dx%d\n", gameW, gameH);

    // ===[ Main Loop ]===
    uint64_t lastTime = mftb();
    double accumulator = 0.0;

    // Parse deferDrawToAfterAllSteps from CONFIG.JSN
    bool deferDrawToAfterAllSteps = false;
    if (configRoot != nullptr) {
        JsonValue* deferDrawVal = JsonReader_getObject(configRoot, "deferDrawToAfterAllSteps");
        if (deferDrawVal != nullptr) {
            deferDrawToAfterAllSteps = JsonReader_getBool(deferDrawVal);
        }
    }

    while (!runner->shouldExit) {
        uint64_t currentTime = mftb();
        double deltaTime = (double) (currentTime - lastTime) / (double) PPC_TIMEBASE_FREQ;
        lastTime = currentTime;

        // ===[ Poll Controller ]===
        usb_do_poll();
        struct controller_data_s controller;

        if (get_controller_data(&controller, 0)) {
            uint32_t buttons = packControllerButtons(&controller);

            repeat(xpadMappingCount, i) {
                uint32_t mask = xpadMappings[i].xpadButton;
                int32_t gmlKey = xpadMappings[i].gmlKey;

                bool wasPressed = (prevButtons & mask) != 0;
                bool isPressed = (buttons & mask) != 0;

                if (isPressed && !wasPressed) {
                    RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                } else if (!isPressed && wasPressed) {
                    RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
                }
            }

            prevButtons = buttons;
        }

        // R trigger removes speed cap (run at full frame rate)
        bool speedCapRemoved = controller.rt > 128;

        // ===[ Game Logic (accumulator-based frame pacing) ]===
        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / roomSpeed) : (1.0 / 60.0);

        accumulator += deltaTime;

        // Cap accumulator to prevent spiral of death (max 4 game frames per vsync)
        double maxAccumulator = targetFrameTime * 4.0;
        if (accumulator > maxAccumulator) {
            accumulator = maxAccumulator;
        }

        // If right trigger is held, force at least one game frame per vsync
        if (speedCapRemoved) {
            if (targetFrameTime > accumulator) {
                accumulator = targetFrameTime;
            }
        }

        int gameFramesRan = 0;
        while (accumulator >= targetFrameTime) {
            if (gameFramesRan > 0)
                RunnerKeyboard_beginFrame(runner->keyboard);

            Runner_step(runner);

            if (!deferDrawToAfterAllSteps) {
                renderer->vtable->beginFrame(renderer, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

                // Clear with room background color
                if (runner->drawBackgroundColor) {
                    // The Xenos clear color is set via Xe_SetClearColor in beginFrame
                    // For now, the background is drawn as a full-screen rectangle
                    uint8_t bgR = BGR_R(runner->backgroundColor);
                    uint8_t bgG = BGR_G(runner->backgroundColor);
                    uint8_t bgB = BGR_B(runner->backgroundColor);
                    (void) bgR; (void) bgG; (void) bgB;
                    // TODO: Draw background color rectangle
                }

                // Render views
                Room* activeRoom = runner->currentRoom;
                bool anyViewRendered = false;
                bool viewsEnabled = (activeRoom->flags & 1) != 0;

                if (viewsEnabled) {
                    repeat(8, vi) {
                        if (!activeRoom->views[vi].enabled) continue;

                        int32_t viewX = activeRoom->views[vi].viewX;
                        int32_t viewY = activeRoom->views[vi].viewY;
                        int32_t viewW = activeRoom->views[vi].viewWidth;
                        int32_t viewH = activeRoom->views[vi].viewHeight;
                        int32_t portX = activeRoom->views[vi].portX;
                        int32_t portY = activeRoom->views[vi].portY;
                        int32_t portW = activeRoom->views[vi].portWidth;
                        int32_t portH = activeRoom->views[vi].portHeight;
                        float viewAngle = runner->viewAngles[vi];

                        runner->viewCurrent = (int32_t) vi;
                        renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);
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

        // When deferDrawToAfterAllSteps is enabled, render once after all catch-up steps
        if (deferDrawToAfterAllSteps && gameFramesRan > 0) {
            renderer->vtable->beginFrame(renderer, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

            Room* activeRoom = runner->currentRoom;
            bool anyViewRendered = false;
            bool viewsEnabled = (activeRoom->flags & 1) != 0;

            if (viewsEnabled) {
                repeat(8, vi) {
                    if (!activeRoom->views[vi].enabled) continue;

                    int32_t viewX = activeRoom->views[vi].viewX;
                    int32_t viewY = activeRoom->views[vi].viewY;
                    int32_t viewW = activeRoom->views[vi].viewWidth;
                    int32_t viewH = activeRoom->views[vi].viewHeight;
                    int32_t portX = activeRoom->views[vi].portX;
                    int32_t portY = activeRoom->views[vi].portY;
                    int32_t portW = activeRoom->views[vi].portWidth;
                    int32_t portH = activeRoom->views[vi].portHeight;
                    float viewAngle = runner->viewAngles[vi];

                    runner->viewCurrent = (int32_t) vi;
                    renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);
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

        // Update audio system
        if (runner->audioSystem != nullptr) {
            float dt = (float) deltaTime;
            if (0.0f > dt) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }

        if (gameFramesRan > 0) {
            // Clear input after both Step and Draw have consumed it
            RunnerKeyboard_beginFrame(runner->keyboard);

            // Present the frame (Xe_Resolve + Xe_Sync handled in endFrame)
        }
    }

    // ===[ Cleanup ]===
    if (runner->audioSystem != nullptr) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
    }
    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);

    return 0;
}
