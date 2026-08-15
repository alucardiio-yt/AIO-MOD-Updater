/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019-2020  natinusala
    Copyright (C) 2019  p-sam
    Copyright (C) 2020  WerWolv

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <borealis.hpp>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#define GLM_FORCE_PURE
#define GLM_ENABLE_EXPERIMENTAL
#include <nanovg/nanovg.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg/nanovg_gl.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <chrono>
#include <set>
#include <thread>

constexpr uint32_t WINDOW_WIDTH  = 1280;
constexpr uint32_t WINDOW_HEIGHT = 720;

#define DEFAULT_FPS 60
#define BUTTON_REPEAT_DELAY 15
#define BUTTON_REPEAT_CADENCY 5

using namespace brls::i18n::literals;

namespace brls
{

static void windowFramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    if (!width || !height)
        return;

    glViewport(0, 0, width, height);
    Application::windowScale = (float)width / (float)WINDOW_WIDTH;

    float contentHeight = ((float)height / (Application::windowScale * (float)WINDOW_HEIGHT)) * (float)WINDOW_HEIGHT;

    Application::contentWidth  = WINDOW_WIDTH;
    Application::contentHeight = (unsigned)roundf(contentHeight);

    Application::resizeNotificationManager();

    Logger::info("Window size changed to {}x{}", width, height);
    Logger::info("New scale factor is {}", Application::windowScale);
}

static void joystickCallback(int jid, int event)
{
    if (event == GLFW_CONNECTED)
    {
        Logger::info("Joystick {} connected", jid);
        if (glfwJoystickIsGamepad(jid))
            Logger::info("Joystick {} is gamepad: \"{}\"", jid, glfwGetGamepadName(jid));
    }
    else if (event == GLFW_DISCONNECTED)
        Logger::info("Joystick {} disconnected", jid);
}

static void errorCallback(int errorCode, const char* description)
{
    Logger::error("[GLFW:{}] {}", errorCode, description);
}

static void windowKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
        {
            static int saved_x, saved_y, saved_width, saved_height;

            if (!glfwGetWindowMonitor(window))
            {
                glfwGetWindowPos(window, &saved_x, &saved_y);
                glfwGetWindowSize(window, &saved_width, &saved_height);

                GLFWmonitor* monitor    = glfwGetPrimaryMonitor();
                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
            }
            else
            {
                glfwSetWindowMonitor(window, nullptr, saved_x, saved_y, saved_width, saved_height, GLFW_DONT_CARE);
            }
        }
    }
}

bool Application::init(std::string title, Style* style, LibraryViewsThemeVariantsWrapper* themeVariantsWrapper)
{
    std::srand(std::time(nullptr));

    Application::taskManager         = new TaskManager();
    Application::notificationManager = new NotificationManager();

    Application::currentFocus = nullptr;
    Application::oldGamepad   = {};
    Application::gamepad      = {};
    Application::title        = title;

    if (!themeVariantsWrapper)
        themeVariantsWrapper = new LibraryViewsThemeVariantsWrapper(new HorizonLightTheme(), new HorizonDarkTheme());

    if (!style)
        style = new HorizonStyle();

    Application::currentThemeVariantsWrapper = themeVariantsWrapper;
    Application::currentStyle                = style;

    glfwSetErrorCallback(errorCallback);
    glfwInitHint(GLFW_JOYSTICK_HAT_BUTTONS, GLFW_FALSE);
    if (!glfwInit())
    {
        Logger::error("Failed to initialize glfw");
        return false;
    }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

    Application::window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, title.c_str(), nullptr, nullptr);
    if (!window)
    {
        Logger::error("glfw: failed to create window");
        glfwTerminate();
        return false;
    }

    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, windowFramebufferSizeCallback);
    glfwSetKeyCallback(window, windowKeyCallback);
    glfwSetJoystickCallback(joystickCallback);

    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glfwSwapInterval(1);

    Logger::info("GL Vendor: {}", glGetString(GL_VENDOR));
    Logger::info("GL Renderer: {}", glGetString(GL_RENDERER));
    Logger::info("GL Version: {}", glGetString(GL_VERSION));

    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1))
    {
        GLFWgamepadstate state;
        Logger::info("Gamepad detected: {}", glfwGetGamepadName(GLFW_JOYSTICK_1));
        glfwGetGamepadState(GLFW_JOYSTICK_1, &state);
    }

    Application::vg = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
    if (!vg)
    {
        Logger::error("Unable to init nanovg");
        glfwTerminate();
        return false;
    }

    windowFramebufferSizeCallback(window, WINDOW_WIDTH, WINDOW_HEIGHT);
    glfwSetTime(0.0);

#ifdef __SWITCH__
    {
        PlFontData font;
        SetLanguage localeID = (SetLanguage)i18n::nxGetCurrentLocaleID();

        Result rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
        if (R_SUCCEEDED(rc))
        {
            Logger::info("Adding Switch shared standard font");
            Application::fontStash.standard = Application::loadFontFromMemory("standard", font.address, font.size, false);
        }

        bool isFullFallback = false;
        AppletType at = appletGetAppletType();
        if (at == AppletType_Application || at == AppletType_SystemApplication)
        {
            isFullFallback = true;
            Logger::info("Non applet mode, font full fallback is enabled!");
        }

        if (localeID == SetLanguage_ZHCN || localeID == SetLanguage_ZHHANS || isFullFallback)
        {
            rc = plGetSharedFontByType(&font, PlSharedFontType_ChineseSimplified);
            if (R_SUCCEEDED(rc))
            {
                Logger::info("Adding Switch shared S.Chinese font");
                Application::fontStash.schinese = Application::loadFontFromMemory("schinese", font.address, font.size, false);
            }

            rc = plGetSharedFontByType(&font, PlSharedFontType_ExtChineseSimplified);
            if (R_SUCCEEDED(rc))
            {
                Logger::info("Adding Switch shared S.Chinese extended font");
                Application::fontStash.extSchinese = Application::loadFontFromMemory("extSchinese", font.address, font.size, false);
            }
        }

        if (localeID == SetLanguage_ZHTW || localeID == SetLanguage_ZHHANT || isFullFallback)
        {
            rc = plGetSharedFontByType(&font, PlSharedFontType_ChineseTraditional);
            if (R_SUCCEEDED(rc))
            {
                Logger::info("Adding Switch shared T.Chinese font");
                Application::fontStash.tchinese = Application::loadFontFromMemory("tchinese", font.address, font.size, false);
            }
        }

        if (localeID == SetLanguage_KO || isFullFallback)
        {
            rc = plGetSharedFontByType(&font, PlSharedFontType_KO);
            if (R_SUCCEEDED(rc))
            {
                Logger::info("Adding Switch shared Korean font");
                Application::fontStash.korean = Application::loadFontFromMemory("korean", font.address, font.size, false);
            }
        }

        switch (localeID)
        {
            case SetLanguage_ZHCN:
            case SetLanguage_ZHHANS:
                if (isFullFallback)
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.extSchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.tchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.standard);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.korean);
                }
                else
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.extSchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.schinese, Application::fontStash.standard);
                }
                Logger::info("Using Switch shared S.Chinese font as regular");
                Application::fontStash.regular = Application::fontStash.schinese;
                break;

            case SetLanguage_ZHTW:
            case SetLanguage_ZHHANT:
                if (isFullFallback)
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.tchinese, Application::fontStash.schinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.tchinese, Application::fontStash.extSchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.tchinese, Application::fontStash.standard);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.tchinese, Application::fontStash.korean);
                }
                else
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.tchinese, Application::fontStash.standard);
                }
                Logger::info("Using Switch shared T.Chinese font as regular");
                Application::fontStash.regular = Application::fontStash.tchinese;
                break;

            case SetLanguage_KO:
                if (isFullFallback)
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.korean, Application::fontStash.standard);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.korean, Application::fontStash.schinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.korean, Application::fontStash.extSchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.korean, Application::fontStash.tchinese);
                }
                else
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.korean, Application::fontStash.standard);
                }
                Logger::info("Using Switch shared Korean font as regular");
                Application::fontStash.regular = Application::fontStash.korean;
                break;

            default:
                if (isFullFallback)
                {
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.standard, Application::fontStash.schinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.standard, Application::fontStash.extSchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.standard, Application::fontStash.tchinese);
                    nvgAddFallbackFontId(Application::vg, Application::fontStash.standard, Application::fontStash.korean);
                }
                Logger::info("Using Switch shared standard font as regular");
                Application::fontStash.regular = Application::fontStash.standard;
                break;
        }

        rc = plGetSharedFontByType(&font, PlSharedFontType_NintendoExt);
        if (R_SUCCEEDED(rc))
        {
            Logger::info("Using Switch shared symbols font");
            Application::fontStash.sharedSymbols = Application::loadFontFromMemory("symbols", font.address, font.size, false);
        }
    }
#else
    if (access(BOREALIS_ASSET("Illegal-Font.ttf"), F_OK) != -1)
        Application::fontStash.regular = Application::loadFont("regular", BOREALIS_ASSET("Illegal-Font.ttf"));
    else
        Application::fontStash.regular = Application::loadFont("regular", BOREALIS_ASSET("inter/Inter-Switch.ttf"));

    if (Application::fontStash.regular == -1)
        brls::Logger::warning("Couldn't load regular font, no text will be displayed!");

    if (access(BOREALIS_ASSET("Wingdings.ttf"), F_OK) != -1)
        Application::fontStash.sharedSymbols = Application::loadFont("sharedSymbols", BOREALIS_ASSET("Wingdings.ttf"));
#endif

    if (access(BOREALIS_ASSET("material/MaterialIcons-Regular.ttf"), F_OK) != -1)
        Application::fontStash.material = Application::loadFont("material", BOREALIS_ASSET("material/MaterialIcons-Regular.ttf"));

    if (Application::fontStash.sharedSymbols)
    {
        Logger::info("Using shared symbols font");
        nvgAddFallbackFontId(Application::vg, Application::fontStash.regular, Application::fontStash.sharedSymbols);
    }
    else
    {
        Logger::warning("Shared symbols font not found");
    }

    if (Application::fontStash.material)
    {
        Logger::info("Using Material font");
        nvgAddFallbackFontId(Application::vg, Application::fontStash.regular, Application::fontStash.material);
    }
    else
    {
        Logger::warning("Material font not found");
    }

#ifdef __SWITCH__
    ColorSetId nxTheme;
    setsysGetColorSetId(&nxTheme);

    if (nxTheme == ColorSetId_Dark)
        Application::currentThemeVariant = ThemeVariant::DARK;
    else
        Application::currentThemeVariant = ThemeVariant::LIGHT;
#else
    char* themeEnv = getenv("BOREALIS_THEME");
    if (themeEnv != nullptr && !strcasecmp(themeEnv, "DARK"))
        Application::currentThemeVariant = ThemeVariant::DARK;
    else
        Application::currentThemeVariant = ThemeVariant::LIGHT;
#endif

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    Application::windowWidth  = viewport[2];
    Application::windowHeight = viewport[3];

    menu_animation_init();

    Application::setMaximumFPS(DEFAULT_FPS);

    return true;
}

bool Application::mainLoop()
{
    retro_time_t frameStart = 0;
    if (Application::frameTime > 0.0f)
        frameStart = cpu_features_get_time_usec();

    bool is_active;
    do
    {
        is_active = !glfwGetWindowAttrib(Application::window, GLFW_ICONIFIED);
        if (is_active)
            glfwPollEvents();
        else
            glfwWaitEvents();

        if (glfwWindowShouldClose(Application::window))
        {
            Application::exit();
            return false;
        }
    } while (!is_active);

#ifdef __SWITCH__
    if (!appletMainLoop())
    {
        Application::exit();
        return false;
    }
#endif

    if (!glfwGetGamepadState(GLFW_JOYSTICK_1, &Application::gamepad))
    {
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]    = glfwGetKey(window, GLFW_KEY_LEFT);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]   = glfwGetKey(window, GLFW_KEY_RIGHT);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]      = glfwGetKey(window, GLFW_KEY_UP);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]    = glfwGetKey(window, GLFW_KEY_DOWN);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_START]        = glfwGetKey(window, GLFW_KEY_ESCAPE);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_BACK]         = glfwGetKey(window, GLFW_KEY_F1);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_A]            = glfwGetKey(window, GLFW_KEY_ENTER);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_B]            = glfwGetKey(window, GLFW_KEY_BACKSPACE);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER]  = glfwGetKey(window, GLFW_KEY_L);
        Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = glfwGetKey(window, GLFW_KEY_R);
    }

    int count;
    const float* axes = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &count);

    if (count >= 4)
    {
        if (axes[0] < -.5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = GLFW_PRESS;
        else if (axes[0] > .5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = GLFW_PRESS;

        if (axes[1] < -.5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] = GLFW_PRESS;
        else if (axes[1] > .5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] = GLFW_PRESS;

        if (axes[2] < -.5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = GLFW_PRESS;
        else if (axes[2] > .5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = GLFW_PRESS;

        if (axes[3] < -.5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] = GLFW_PRESS;
        else if (axes[3] > .5)
            Application::gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] = GLFW_PRESS;
    }

    bool anyButtonPressed               = false;
    bool repeating                      = false;
    static retro_time_t buttonPressTime = 0;
    static int repeatingButtonTimer     = 0;

    for (int i = GLFW_GAMEPAD_BUTTON_A; i <= GLFW_GAMEPAD_BUTTON_LAST; i++)
    {
        if (Application::gamepad.buttons[i] == GLFW_PRESS)
        {
            anyButtonPressed = true;
            repeating        = (repeatingButtonTimer > BUTTON_REPEAT_DELAY && repeatingButtonTimer % BUTTON_REPEAT_CADENCY == 0);

            if (Application::oldGamepad.buttons[i] != GLFW_PRESS || repeating)
                Application::onGamepadButtonPressed(i, repeating);
        }

        if (Application::gamepad.buttons[i] != Application::oldGamepad.buttons[i])
            buttonPressTime = repeatingButtonTimer = 0;
    }

    if (anyButtonPressed && cpu_features_get_time_usec() - buttonPressTime > 1000)
    {
        buttonPressTime = cpu_features_get_time_usec();
        repeatingButtonTimer++;
    }

    Application::oldGamepad = Application::gamepad;

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    unsigned newWidth  = viewport[2];
    unsigned newHeight = viewport[3];

    if (Application::windowWidth != newWidth || Application::windowHeight != newHeight)
    {
        Application::windowWidth  = newWidth;
        Application::windowHeight = newHeight;
        Application::onWindowSizeChanged();
    }

    menu_animation_update();

    Application::taskManager->frame();

    Application::frame();
    glfwSwapBuffers(window);

    if (Application::frameTime > 0.0f)
    {
        retro_time_t currentFrameTime = cpu_features_get_time_usec() - frameStart;
        retro_time_t frameTime        = (retro_time_t)(Application::frameTime * 1000);

        if (frameTime > currentFrameTime)
        {
            retro_time_t toSleep = frameTime - currentFrameTime;
            std::this_thread::sleep_for(std::chrono::microseconds(toSleep));
        }
    }

    Application::focusLocked = false;

    return true;
}

void Application::quit()
{
    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

void Application::navigate(FocusDirection direction)
{
    if (Application::focusLocked)
        return;

    View* currentFocus = Application::currentFocus;

    if (!currentFocus || !currentFocus->hasParent())
        return;

    View* nextFocus = currentFocus->getParent()->getNextFocus(direction, currentFocus);

    while (!nextFocus)
    {
        if (!currentFocus->hasParent() || !currentFocus->getParent()->hasParent())
            break;

        currentFocus = currentFocus->getParent();
        nextFocus    = currentFocus->getParent()->getNextFocus(direction, currentFocus);
    }

    if (!nextFocus)
    {
        Application::currentFocus->shakeHighlight(direction);
        return;
    }

    Application::focusLocked = true;
    Application::giveFocus(nextFocus);
}

void Application::onGamepadButtonPressed(char button, bool repeating)
{
    if (Application::blockInputsTokens != 0)
        return;

    if (repeating && Application::repetitionOldFocus == Application::currentFocus)
        return;

    Application::repetitionOldFocus = Application::currentFocus;

    if (Application::currentFocus && button == GLFW_GAMEPAD_BUTTON_A)
        Application::currentFocus->playClickAnimation();

    if (Application::handleAction(button))
        return;

    switch (button)
    {
        case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:
            Application::navigate(FocusDirection::DOWN);
            break;
        case GLFW_GAMEPAD_BUTTON_DPAD_UP:
            Application::navigate(FocusDirection::UP);
            break;
        case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:
            Application::navigate(FocusDirection::LEFT);
            break;
        case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:
            Application::navigate(FocusDirection::RIGHT);
            break;
        default:
            break;
    }
}

View* Application::getCurrentFocus()
{
    return Application::currentFocus;
}

bool Application::handleAction(char button)
{
    if (Application::viewStack.empty())
        return false;

    View* hintParent = Application::currentFocus;
    std::set<Key> consumedKeys;

    if (!hintParent)
        hintParent = Application::viewStack[Application::viewStack.size() - 1];

    while (hintParent)
    {
        for (auto& action : hintParent->getActions())
        {
            if (action.key != static_cast<Key>(button))
                continue;

            if (consumedKeys.find(action.key) != consumedKeys.end())
                continue;

            if (action.available)
                if (action.actionListener())
                    consumedKeys.insert(action.key);
        }

        hintParent = hintParent->getParent();
    }

    return !consumedKeys.empty();
}

void Application::frame()
{
    FrameContext frameContext = FrameContext();

    frameContext.pixelRatio = (float)Application::windowWidth / (float)Application::windowHeight;
    frameContext.vg         = Application::vg;
    frameContext.fontStash  = &Application::fontStash;
    frameContext.theme      = Application::getTheme();

    glClearColor(
        frameContext.theme->backgroundColor[0],
        frameContext.theme->backgroundColor[1],
        frameContext.theme->backgroundColor[2],
        1.0f);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    if (Application::background)
        Application::background->preFrame();

    nvgBeginFrame(Application::vg, Application::windowWidth, Application::windowHeight, frameContext.pixelRatio);
    nvgScale(Application::vg, Application::windowScale, Application::windowScale);

    float w = Application::contentWidth;
    float h = Application::contentHeight;

    NVGpaint bgGradient = nvgLinearGradient(
        Application::vg,
        0, 0,
        w, h,
        nvgRGBA(3, 3, 6, 255),
        nvgRGBA(20, 10, 35, 255));

    nvgBeginPath(Application::vg);
    nvgRect(Application::vg, 0, 0, w, h);
    nvgFillPaint(Application::vg, bgGradient);
    nvgFill(Application::vg);

    NVGpaint glowTopRight = nvgRadialGradient(
        Application::vg,
        w * 0.88f, h * 0.12f,
        5.0f, 180.0f,
        nvgRGBA(140, 70, 255, 35),
        nvgRGBA(140, 70, 255, 0));

    nvgBeginPath(Application::vg);
    nvgRect(Application::vg, 0, 0, w, h);
    nvgFillPaint(Application::vg, glowTopRight);
    nvgFill(Application::vg);

    NVGpaint glowBottomLeft = nvgRadialGradient(
        Application::vg,
        w * 0.18f, h * 0.88f,
        5.0f, 220.0f,
        nvgRGBA(90, 40, 180, 20),
        nvgRGBA(90, 40, 180, 0));

    nvgBeginPath(Application::vg);
    nvgRect(Application::vg, 0, 0, w, h);
    nvgFillPaint(Application::vg, glowBottomLeft);
    nvgFill(Application::vg);

    std::vector<View*> viewsToDraw;

    for (size_t i = 0; i < Application::viewStack.size(); i++)
    {
        View* view = Application::viewStack[Application::viewStack.size() - 1 - i];
        viewsToDraw.push_back(view);

        if (!view->isTranslucent())
            break;
    }

    if (Application::background)
        Application::background->frame(&frameContext);

    for (size_t i = 0; i < viewsToDraw.size(); i++)
    {
        View* view = viewsToDraw[viewsToDraw.size() - 1 - i];
        view->frame(&frameContext);
    }

    if (Application::framerateCounter)
        Application::framerateCounter->frame(&frameContext);

    Application::notificationManager->frame(&frameContext);

    nvgResetTransform(Application::vg);
    nvgEndFrame(Application::vg);

    if (Application::background)
        Application::background->postFrame();
}

void Application::exit()
{
    Application::clear();

    if (Application::vg)
        nvgDeleteGL3(Application::vg);

    glfwTerminate();

    menu_animation_free();

    if (Application::framerateCounter)
        delete Application::framerateCounter;

    delete Application::taskManager;
    delete Application::notificationManager;

    delete Application::currentThemeVariantsWrapper;
    delete Application::currentStyle;
}

void Application::setDisplayFramerate(bool enabled)
{
    if (!Application::framerateCounter && enabled)
    {
        Logger::debug("Enabling framerate counter");
        Application::framerateCounter = new FramerateCounter();
        Application::resizeFramerateCounter();
    }
    else if (Application::framerateCounter && !enabled)
    {
        Logger::debug("Disabling framerate counter");
        delete Application::framerateCounter;
        Application::framerateCounter = nullptr;
    }
}

void Application::toggleFramerateDisplay()
{
    Application::setDisplayFramerate(!Application::framerateCounter);
}

void Application::resizeFramerateCounter()
{
    if (!Application::framerateCounter)
        return;

    Style* style                   = Application::getStyle();
    unsigned framerateCounterWidth = style->FramerateCounter.width;
    unsigned width                 = WINDOW_WIDTH;

    Application::framerateCounter->setBoundaries(
        width - framerateCounterWidth,
        0,
        framerateCounterWidth,
        style->FramerateCounter.height);
    Application::framerateCounter->invalidate();
}

void Application::resizeNotificationManager()
{
    Application::notificationManager->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);
    Application::notificationManager->invalidate();
}

void Application::notify(std::string text)
{
    Application::notificationManager->notify(text);
}

NotificationManager* Application::getNotificationManager()
{
    return Application::notificationManager;
}

void Application::giveFocus(View* view)
{
    View* oldFocus = Application::currentFocus;
    View* newFocus = view ? view->getDefaultFocus() : nullptr;

    if (oldFocus != newFocus)
    {
        if (oldFocus)
            oldFocus->onFocusLost();

        Application::currentFocus = newFocus;
        Application::globalFocusChangeEvent.fire(newFocus);

        if (newFocus)
        {
            newFocus->onFocusGained();
            Logger::debug("Giving focus to {}", newFocus->describe());
        }
    }
}

void Application::popView(ViewAnimation animation, std::function<void(void)> cb)
{
    if (Application::viewStack.size() <= 1)
        return;

    Application::blockInputs();

    View* last = Application::viewStack[Application::viewStack.size() - 1];
    last->willDisappear(true);

    last->setForceTranslucent(true);

    bool wait = animation == ViewAnimation::FADE;

    last->hide([last, animation, wait, cb]() {
        last->setForceTranslucent(false);
        Application::viewStack.pop_back();
        delete last;

        if (Application::viewStack.size() > 0 && wait)
        {
            View* newLast = Application::viewStack[Application::viewStack.size() - 1];

            if (newLast->isHidden())
            {
                newLast->willAppear(false);
                newLast->show(cb, true, animation);
            }
            else
            {
                cb();
            }
        }

        Application::unblockInputs();
    },
        true, animation);

    if (!wait && Application::viewStack.size() > 1)
    {
        View* toShow = Application::viewStack[Application::viewStack.size() - 2];
        toShow->willAppear(false);
        toShow->show(cb, true, animation);
    }

    if (Application::focusStack.size() > 0)
    {
        View* newFocus = Application::focusStack[Application::focusStack.size() - 1];

        Logger::debug("Giving focus to {}, and removing it from the focus stack", newFocus->describe());

        Application::giveFocus(newFocus);
        Application::focusStack.pop_back();
    }
}

void Application::pushView(View* view, ViewAnimation animation)
{
    Application::blockInputs();

    View* last = nullptr;
    if (Application::viewStack.size() > 0)
        last = Application::viewStack[Application::viewStack.size() - 1];

    bool fadeOut = last && !last->isTranslucent() && !view->isTranslucent();
    bool wait    = animation == ViewAnimation::FADE;

    view->registerAction("brls/hints/exit"_i18n, Key::PLUS, [] { Application::quit(); return true; });
    view->registerAction(
        "FPS", Key::MINUS, [] { Application::toggleFramerateDisplay(); return true; }, true);

    if (fadeOut)
    {
        view->setForceTranslucent(true);

        if (!wait)
        {
            view->show([]() {
                Application::unblockInputs();
            },
                true, animation);
        }

        last->hide([animation, wait]() {
            View* newLast = Application::viewStack[Application::viewStack.size() - 1];
            newLast->setForceTranslucent(false);

            if (wait)
                newLast->show([]() { Application::unblockInputs(); }, true, animation);
        },
            true, animation);
    }

    view->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);

    if (!fadeOut)
        view->show([]() { Application::unblockInputs(); }, true, animation);
    else
        view->alpha = 0.0f;

    if (Application::viewStack.size() > 0 && Application::currentFocus != nullptr)
    {
        Logger::debug("Pushing {} to the focus stack", Application::currentFocus->describe());
        Application::focusStack.push_back(Application::currentFocus);
    }

    view->invalidate(true);
    view->willAppear(true);
    Application::giveFocus(view->getDefaultFocus());

    Application::viewStack.push_back(view);
}

void Application::onWindowSizeChanged()
{
    Logger::debug("Layout triggered");

    for (View* view : Application::viewStack)
    {
        view->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);
        view->invalidate();

        view->onWindowSizeChanged();
    }

    if (Application::background)
    {
        Application::background->setBoundaries(
            0,
            0,
            Application::contentWidth,
            Application::contentHeight);

        Application::background->invalidate();
        Application::background->onWindowSizeChanged();
    }

    Application::resizeNotificationManager();
    Application::resizeFramerateCounter();
}

void Application::clear()
{
    for (View* view : Application::viewStack)
    {
        view->willDisappear(true);
        delete view;
    }

    Application::viewStack.clear();
}

Style* Application::getStyle()
{
    return Application::currentStyle;
}

Theme* Application::getTheme()
{
    return Application::currentThemeVariantsWrapper->getTheme(Application::currentThemeVariant);
}

LibraryViewsThemeVariantsWrapper* Application::getThemeVariantsWrapper()
{
    return Application::currentThemeVariantsWrapper;
}

ThemeVariant Application::getThemeVariant()
{
    return Application::currentThemeVariant;
}

int Application::loadFont(const char* fontName, const char* filePath)
{
    return nvgCreateFont(Application::vg, fontName, filePath);
}

int Application::loadFontFromMemory(const char* fontName, void* address, size_t size, bool freeData)
{
    return nvgCreateFontMem(Application::vg, fontName, (unsigned char*)address, size, freeData);
}

int Application::findFont(const char* fontName)
{
    return nvgFindFont(Application::vg, fontName);
}

void Application::crash(std::string text)
{
    CrashFrame* crashFrame = new CrashFrame(text);
    Application::pushView(crashFrame);
}

void Application::blockInputs()
{
    Application::blockInputsTokens += 1;
}

void Application::unblockInputs()
{
    if (Application::blockInputsTokens > 0)
        Application::blockInputsTokens -= 1;
}

NVGcontext* Application::getNVGContext()
{
    return Application::vg;
}

TaskManager* Application::getTaskManager()
{
    return Application::taskManager;
}

void Application::setCommonFooter(std::string footer)
{
    Application::commonFooter = footer;
}

std::string* Application::getCommonFooter()
{
    return &Application::commonFooter;
}

FramerateCounter::FramerateCounter()
    : Label(LabelStyle::FPS, "FPS: ---")
{
    this->setColor(nvgRGB(255, 255, 255));
    this->setVerticalAlign(NVG_ALIGN_MIDDLE);
    this->setHorizontalAlign(NVG_ALIGN_RIGHT);
    this->setBackground(ViewBackground::BACKDROP);

    this->lastSecond = cpu_features_get_time_usec() / 1000;
}

void FramerateCounter::frame(FrameContext* ctx)
{
    retro_time_t current = cpu_features_get_time_usec() / 1000;

    if (current - this->lastSecond >= 1000)
    {
        char fps[10];
        snprintf(fps, sizeof(fps), "FPS: %03d", this->frames);
        this->setText(std::string(fps));
        this->invalidate();

        this->frames     = 0;
        this->lastSecond = current;
    }

    this->frames++;

    Label::frame(ctx);
}

void Application::setMaximumFPS(unsigned fps)
{
    if (fps == 0)
        Application::frameTime = 0.0f;
    else
        Application::frameTime = 1000 / (float)fps;

    Logger::info("Maximum FPS set to {} - using a frame time of {:.2f} ms", fps, Application::frameTime);
}

std::string Application::getTitle()
{
    return Application::title;
}

GenericEvent* Application::getGlobalFocusChangeEvent()
{
    return &Application::globalFocusChangeEvent;
}

VoidEvent* Application::getGlobalHintsUpdateEvent()
{
    return &Application::globalHintsUpdateEvent;
}

FontStash* Application::getFontStash()
{
    return &Application::fontStash;
}

void Application::setBackground(Background* background)
{
    if (Application::background)
    {
        Application::background->willDisappear();
        delete Application::background;
    }

    Application::background = background;

    background->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);
    background->invalidate(true);
    background->willAppear(true);
}

void Application::cleanupNvgGlState()
{
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
}

} // namespace brls