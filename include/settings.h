#pragma once
#include <Preferences.h>

struct Settings {
    bool detect_drone   = true;
    bool detect_tracker = true;
    bool detect_camera  = true;
    bool detect_bodycam = true;
    bool detect_attack  = true;
    bool speaker_en     = true;
    bool led_en         = true;
    uint8_t brightness  = 200;
};

inline void settings_save(const Settings& s) {
    Preferences p; p.begin("ss", false);
    p.putBool("drone",   s.detect_drone);
    p.putBool("tracker", s.detect_tracker);
    p.putBool("camera",  s.detect_camera);
    p.putBool("bodycam", s.detect_bodycam);
    p.putBool("attack",  s.detect_attack);
    p.putBool("spk",     s.speaker_en);
    p.putBool("led",     s.led_en);
    p.putUChar("brt",    s.brightness);
    p.end();
}

inline void settings_load(Settings& s) {
    Preferences p; p.begin("ss", true);
    s.detect_drone   = p.getBool("drone",   true);
    s.detect_tracker = p.getBool("tracker", true);
    s.detect_camera  = p.getBool("camera",  true);
    s.detect_bodycam = p.getBool("bodycam", true);
    s.detect_attack  = p.getBool("attack",  true);
    s.speaker_en     = p.getBool("spk",     true);
    s.led_en         = p.getBool("led",     true);
    s.brightness     = p.getUChar("brt",    200);
    p.end();
}
