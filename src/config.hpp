#pragma once

#include <string>
#include <fstream>
#include <algorithm>
#include <iostream>

struct FaceAuthConfig {
    std::string liveness_method = "minifas"; // "minifas" (default passive AI), "texture", "none"
    double matching_threshold = 0.45;
    
    // Passive texture liveness check (Laplacian/HSV)
    double texture_min_laplacian = 380.0;     // Protects against blurry prints
    double texture_max_laplacian = 1500.0;   // Protects against screen Moire pattern grids
    double texture_min_hsv_sat = 95.0;       // Skin saturation minimum
    double texture_max_hsv_sat = 190.0;      // Skin saturation maximum

    // MiniFASNet AI anti-spoofing threshold
    double minifas_threshold = 1.5;

    // Environmental constraints
    double min_brightness = 35.0;
    bool enable_adaptive_exposure = true;

    // Active liveness thresholds
    double active_straight_min = 0.45;
    double active_straight_max = 0.55;
    double active_turn_left = 0.62;
    double active_turn_right = 0.38;
};

inline std::string trim(std::string str) {
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), str.end());
    return str;
}

inline FaceAuthConfig readFaceAuthConfig(const std::string& filepath = "/etc/faceauth.conf") {
    FaceAuthConfig config;
    std::ifstream file(filepath);
    
    if (!file.is_open()) {
        file.open("faceauth.conf");
        if (!file.is_open()) {
            std::cout << "[CONFIG] Yapilandirma dosyasi bulunamadi (" << filepath << "). Varsayilanlar yukleniyor." << std::endl;
            return config;
        }
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
        comment_pos = line.find(';');
        if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);

        line = trim(line);
        if (line.empty() || line[0] == '[') continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string val = trim(line.substr(eq_pos + 1));

        if (key == "liveness_method") {
            config.liveness_method = val;
        } else if (key == "matching_threshold") {
            try { config.matching_threshold = std::stod(val); } catch (...) {}
        } else if (key == "texture_min_laplacian") {
            try { config.texture_min_laplacian = std::stod(val); } catch (...) {}
        } else if (key == "texture_max_laplacian") {
            try { config.texture_max_laplacian = std::stod(val); } catch (...) {}
        } else if (key == "texture_min_hsv_sat") {
            try { config.texture_min_hsv_sat = std::stod(val); } catch (...) {}
        } else if (key == "texture_max_hsv_sat") {
            try { config.texture_max_hsv_sat = std::stod(val); } catch (...) {}
        } else if (key == "minifas_threshold") {
            try { config.minifas_threshold = std::stod(val); } catch (...) {}
        } else if (key == "min_brightness") {
            try { config.min_brightness = std::stod(val); } catch (...) {}
        } else if (key == "enable_adaptive_exposure") {
            config.enable_adaptive_exposure = (val == "true" || val == "1");
        } else if (key == "active_straight_min") {
            try { config.active_straight_min = std::stod(val); } catch (...) {}
        } else if (key == "active_straight_max") {
            try { config.active_straight_max = std::stod(val); } catch (...) {}
        } else if (key == "active_turn_left") {
            try { config.active_turn_left = std::stod(val); } catch (...) {}
        } else if (key == "active_turn_right") {
            try { config.active_turn_right = std::stod(val); } catch (...) {}
        }
    }

    std::cout << "[CONFIG] Yapilandirma yuklendi:" << std::endl;
    std::cout << "  - Liveness Metodu      : " << config.liveness_method << std::endl;
    std::cout << "  - Eşleşme Eşiği        : " << config.matching_threshold << std::endl;
    std::cout << "  - Doku Min/Max Lap     : " << config.texture_min_laplacian << " / " << config.texture_max_laplacian << std::endl;
    std::cout << "  - Doku Min/Max Sat     : " << config.texture_min_hsv_sat << " / " << config.texture_max_hsv_sat << std::endl;
    std::cout << "  - MiniFASNet Eşiği     : " << config.minifas_threshold << std::endl;
    std::cout << "  - Minimum Parlaklık    : " << config.min_brightness << std::endl;
    std::cout << "  - Adaptif Pozlama (CLAHE): " << (config.enable_adaptive_exposure ? "ACIK" : "KAPALI") << std::endl;

    return config;
}
