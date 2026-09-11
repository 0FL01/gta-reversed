// Independent dictionary lookup/texel oracle and inherited hostile GL snapshot.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define main RealtimeHudOriginalProbeMain
#include "app/platform/linux/RealtimeHudProbe.cpp"
#undef main
#pragma GCC diagnostic pop

struct HudDictionary {
    rw::TexDictionary* Previous = rw::TexDictionary::getCurrent();
    rw::TexDictionary* Value{};
    ~HudDictionary() {
        rw::TexDictionary::setCurrent(Previous);
        if (Value) Value->destroy();
    }
};

static std::vector<std::uint8_t> SpriteSourceBytes() {
    HudFile file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, "models/hud.txd", FILE_ACCESS_READ) == 0 && file.Handle, "source HUD open");
    const auto size = OS_FileSize(file.Handle);
    Require(size > 0, "source HUD size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    Require(OS_FileRead(file.Handle, bytes.data(), size) == 0, "source HUD read");
    return bytes;
}

static SpriteImages SourceImages(const std::vector<std::uint8_t>& bytes) {
    HudDictionary dictionary;
    rw::StreamMemory stream;
    stream.open(const_cast<std::uint8_t*>(bytes.data()), static_cast<uint32>(bytes.size()));
    Require(rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr), "reference dictionary chunk");
    dictionary.Value = rw::TexDictionary::streamRead(&stream);
    stream.close();
    Require(dictionary.Value, "independent whole dictionary parse");
    SpriteImages images;
    for (int i = 2; i < 64; ++i) {
        auto* texture = dictionary.Value->find(RealtimeHud::RadarSpriteName(i));
        Require(texture && TexSample_Decode(texture, images[i]), "original named sprite decode");
        images[i].filter = texture->filterAddressing;
    }
    return images;
}

static void FaultProbe(const std::vector<std::uint8_t>& bytes) {
    SpriteImages images;
    SpriteStates states;
    char error[512]{};
    // Locate the actual sprite33 D3D native header; mutate owned RAM only.
    const std::string_view name = "radar_race";
    const auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end()) - bytes.begin();
    Require(at >= 8 && std::size_t(at + 100) < bytes.size(), "fault fixture original texture name");
    const auto header = at - 8;
    for (int fault = 0; fault < 5; ++fault) {
        auto changed = bytes;
        if (fault == 0) changed[at] = 'x'; // absent name is not a substituted image
        if (fault == 1) changed[header + 4] = 0; // unsupported sampler
        if (fault == 2) changed[header + 80] = changed[header + 81] = 0;
        if (fault == 3) changed.resize(changed.size() - 1);
        if (fault == 4) changed[header + 88] ^= 1; // corrupt native mip size
        auto* previous = rw::TexDictionary::getCurrent();
        const bool ok = DecodeRadarSprites(changed, images, states, error, sizeof(error));
        Require(rw::TexDictionary::getCurrent() == previous, "fault parsing preserves dictionary");
        if (fault < 2) {
            Require(ok && images[33].rgba.empty(), "missing/unsupported sprite produces no substitute");
            Require(states[33] == (fault ? RealtimeHud::RadarSpriteState::Unsupported : RealtimeHud::RadarSpriteState::Missing), "precise asset state");
            Require(states[31] == RealtimeHud::RadarSpriteState::Prepared && states[32] == RealtimeHud::RadarSpriteState::Prepared, "unrelated prepared originals survive absent sprite");
        } else {
            Require(!ok && std::strstr(error, "corrupt"), "corrupt present native asset explicitly fails");
        }
    }
    std::puts("sprite-faults PASS missing, unsupported, zero-dimension, truncation, wrong mip bytes, dictionary preservation");
}

static std::vector<std::uint8_t> SpriteRender(const RealtimeHud& hud, RealtimeHudState state,
    const NativeScriptRadarBlip& blip, int width, int height) {
    const std::array blips{blip};
    state.scriptBlips = blips;
    return Render(hud, {kPi / 2}, state, width, height);
}

static void AllSpritePixels(const RealtimeHud& hud, const SpriteImages& reference) {
    RealtimeHudState state;
    state.playerX = 2495; state.playerY = -1685; state.hour = 12; state.minute = 34;
    NativeScriptRadarBlip blip;
    blip.Active = true; blip.ShortRange = true; blip.Display = 2;
    blip.Kind = NativeScriptBlipKind::Coordinate;
    blip.Position = {state.playerX + state.radarRange * 24 / 47, state.playerY, 200};
    state.playerOnMission = true; // 04CE must not inherit contact mission hiding
    for (int scale : {1, 2}) {
        const int width = 640 * scale, height = 448 * scale;
        const auto base = Render(hud, {kPi / 2}, state, width, height);
        for (int sprite = 2; sprite < 64; ++sprite) {
            blip.Sprite = sprite;
            const auto pixels = SpriteRender(hud, state, blip, width, height);
            const auto& image = reference[sprite];
            const int repeats = sprite <= 4 ? 3 : sprite == 41 ? 2 : 1;
            int bad = 0, outside = 0, ink = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto offset = (std::size_t(height - 1 - y) * width + x) * 4;
                if (x < 103 * scale || x >= 119 * scale || y < 374 * scale || y >= 390 * scale) {
                    outside += !std::equal(pixels.begin() + offset, pixels.begin() + offset + 4, base.begin() + offset);
                    continue;
                }
                const double u = (x + .5 - 103 * scale) / (16 * scale), v = (y + .5 - 374 * scale) / (16 * scale);
                std::array<float, 4> texel;
                if ((image.filter & 0xff) == 1) {
                    const auto pixel = (int(v * image.h) * image.w + int(u * image.w)) * 4;
                    std::copy_n(image.rgba.begin() + pixel, 4, texel.begin());
                } else {
                    texel = Sample(image.rgba, image.w, image.h, u, v);
                }
                const double alpha = texel[3] / 255;
                for (int channel = 0; channel < 4; ++channel) {
                    double expected = base[offset + channel];
                    for (int draw = 0; draw < repeats; ++draw) expected = std::round((channel == 3 ? texel[3] : texel[channel] * alpha) + expected * (1 - alpha));
                    bad += std::abs(pixels[offset + channel] - expected) > 2.5;
                }
                ink += texel[3] != 0;
            }
            std::printf("sprite-pixels id=%d scale=%d draws=%d ink=%d badRGBA=%d outside=%d\n", sprite, scale, repeats, ink, bad, outside);
            Require(ink && !bad && !outside, "all-source-name sprite RGB/alpha/filter/UV/priority oracle and unchanged clock/map");
        }
    }
    const auto baseline = Render(hud, {kPi / 2}, state, 640, 448);
    blip.Sprite = 33;
    const auto visible = SpriteRender(hud, state, blip, 640, 448);
    Require(visible != baseline, "coordinate blip visible on mission");
    for (int fault : {-5, -1, 0, 1, 64, 127, 1000000}) {
        auto invalid = blip; invalid.Sprite = fault;
        Require(SpriteRender(hud, state, invalid, 640, 448) == baseline, "wrong ID never aliases glyphs or property sprites");
    }
    auto changed = blip;
    changed.Colour = 0xff000000; changed.Position.Z = -1000; changed.Size = 100;
    changed.Bright = false; changed.Friendly = true; changed.Fade = true;
    Require(SpriteRender(hud, state, changed, 640, 448) == visible, "source sprite white/255 and size independent of trace colour/height");
    changed = blip; changed.Kind = NativeScriptBlipKind::Contact;
    Require(SpriteRender(hud, state, changed, 640, 448) == baseline, "only contact kind hidden on mission");
    changed = blip; changed.ShortRange = false; changed.Position.X = state.playerX + 360;
    Require(SpriteRender(hud, state, changed, 640, 448) != baseline, "long range rim clamp");
    changed.ShortRange = true;
    Require(SpriteRender(hud, state, changed, 640, 448) == baseline, "short range outside rim hidden");
    for (const float distance : {.999f, 1.0f, 1.001f}) {
        Require(RadarVisible(blip, distance, true, 0, true) == (distance <= 1), "short-range threshold before clamping");
    }
    state.radarZoom = 1;
    Require(SpriteRender(hud, state, blip, 640, 448) == baseline, "source zoom suppresses short range");
    state.radarZoom = 0;
    for (int display : {0, 1}) {
        changed = blip; changed.Display = display;
        Require(SpriteRender(hud, state, changed, 640, 448) == baseline, "non-HUD display draws no sprite");
    }
    for (int sprite = 2; sprite < 64; ++sprite) {
        blip.Sprite = sprite;
        const bool interior = sprite <= 4 || sprite == 25 || sprite == 36 || sprite == 41 || sprite == 44 || sprite == 52;
        Require(RadarVisible(blip, .5f, true, 0, false) == interior, "source interior whitelist");
    }
    RealtimeHudView disabled{kPi / 2};
    disabled.locationsBlips = disabled.contactsBlips = disabled.otherBlips = false;
    for (int sprite = 5; sprite < 64; ++sprite) Require(!DisplayRadarSprite(sprite, -99, true, disabled), "source category toggles");
    std::puts("sprite-eligibility PASS coordinate/contact mission, colour/height independence, NONE/wrongID, interior, short-range, zoom, display, category toggles");
}

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{};
    RealtimeHud hud;
    for (int i = -6; i <= 65; ++i) Require(!hud.IsRadarSpriteUploaded(i) && !hud.PreparedRadarSprite(i), "unloaded HUD never reports sprite readiness");
    Require(!hud.Load("", error, sizeof(error)) && std::strstr(error, "game dir"), "failed load explicitly reported");
    Require(hud.Load(game, error, sizeof(error)), error);
    const auto bytes = SpriteSourceBytes();
    const auto reference = SourceImages(bytes);
    FaultProbe(bytes);
    int prepared = 0;
    for (int i = -6; i <= 65; ++i) {
        const auto* image = hud.PreparedRadarSprite(i);
        Require(!hud.IsRadarSpriteUploaded(i), "CPU images are never GPU ready");
        if (image) {
            Require(image->w == reference[i].w && image->h == reference[i].h &&
                image->rgba == reference[i].rgba && image->filter == reference[i].filter &&
                !std::strcmp(image->name, reference[i].name), "independent original named texture mapping");
            ++prepared;
            std::printf("sprite id=%d source=%s actual=%s size=%dx%d filter=0x%x\n", i,
                hud.RadarSpriteName(i), image->name, image->w, image->h, image->filter);
        } else {
            std::printf("sprite id=%d source=%s state=%d\n", i,
                hud.RadarSpriteName(i) ? hud.RadarSpriteName(i) : "NULL", int(hud.GetRadarSpriteState(i)));
        }
    }
    std::printf("sprite-prepared count=%d\n", prepared);
    Require(prepared == 62, "all actual original radar sprite assets available");
    // Parser ownership ends here. Every following operation uses owned data/GL.
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Require(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint size[]{EGL_WIDTH, 1280, EGL_HEIGHT, 896, EGL_NONE};
    auto surface = eglCreatePbufferSurface(display, config, size);
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, context), "EGL context");
    const auto program = HostileState();
    const auto before = Snapshot();
    glEnable(0xffffffff); // explicit failed upload transaction, no sprite ready
    Require(!hud.Upload(error, sizeof(error)), "upload GL failure reported");
    Require(before == Snapshot(), "failed upload restores GL state");
    for (int i = 2; i < 64; ++i) Require(!hud.IsRadarSpriteUploaded(i), "partial upload cannot publish readiness");
    Require(hud.Upload(error, sizeof(error)), error);
    Require(before == Snapshot(), "successful upload restores GL state");
    for (int i = 2; i < 64; ++i) Require(hud.IsRadarSpriteUploaded(i), "all originals uploaded");
    AllSpritePixels(hud, reference);
    hud.ReleaseGpu();
    for (int i = 2; i < 64; ++i) Require(!hud.IsRadarSpriteUploaded(i) && hud.PreparedRadarSprite(i), "release revokes readiness but retains owned images");
    glUseProgram(0); glDeleteProgram(program);
    Require(glGetError() == GL_NO_ERROR, "release GL errors");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("realtime-hud-sprite-probe PASS 62 actual sprites, readiness/failures, source-relative pixels, hostile GL/depth/stencil preservation");
}
