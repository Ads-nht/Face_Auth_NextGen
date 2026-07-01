#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <grp.h>
#include "config.hpp"

using namespace std;
using namespace cv;

// System Paths
const string SOCKET_PATH = "/run/faceauth/faceauth.sock";

// Persistent Neural Networks in memory
Ptr<FaceDetectorYN> detector;
Ptr<FaceRecognizerSF> recognizer;
dnn::Net liveness_net;
int server_fd = -1;

// Path safety helpers
inline bool file_exists(const string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

string get_model_path(const string& filename) {
    string system_path = "/var/lib/faceauth/models/" + filename;
    if (file_exists(system_path)) {
        return system_path;
    }
    return "models/" + filename;
}

// Graceful Shutdown
void handleSignal(int sig) {
    cout << "\n[INFO] Daemon kapatiliyor... (Sinyal: " << sig << ")" << endl;
    if (server_fd != -1) {
        close(server_fd);
    }
    unlink(SOCKET_PATH.c_str());
    exit(0);
}

// Load registered face database (supports multi-angle and backward-compatible single angle)
bool loadFaceFeatures(Mat& straight, Mat& left, Mat& right, bool& is_multi_angle, double& optimal_crop_factor, double& optimal_threshold, const string& filepath) {
    FileStorage fs(filepath, FileStorage::READ);
    if (!fs.isOpened()) return false;
    
    int multi_flag = 0;
    fs["is_multi_angle"] >> multi_flag;
    is_multi_angle = (multi_flag == 1);
    
    if (is_multi_angle) {
        fs["feature_straight"] >> straight;
        fs["feature_left"] >> left;
        fs["feature_right"] >> right;
    } else {
        fs["feature"] >> straight;
    }

    if (fs["optimal_crop_factor"].isNone()) {
        optimal_crop_factor = 1.6;
    } else {
        fs["optimal_crop_factor"] >> optimal_crop_factor;
    }

    if (fs["optimal_threshold"].isNone()) {
        optimal_threshold = 1.5;
    } else {
        fs["optimal_threshold"] >> optimal_threshold;
    }

    return true;
}

// Micro-Movement / Physiological Jitter Tracker with Rigid Eye-Nose Alignment
struct LandmarkHistory {
    std::vector<std::vector<float>> history; // Stores 4 landmarks x 2 coordinates (x, y) = 8 normalized floats
    const size_t max_size = 15;

    void add(const cv::Mat& face_row, float face_width) {
        if (face_row.empty() || face_width <= 0) return;
        
        // Landmarks indices: 
        // 4,5: Right Eye (x,y)
        // 6,7: Left Eye (x,y)
        // 8,9: Nose Tip (x,y)
        // 10,11: Right Mouth Corner (x,y)
        // 12,13: Left Mouth Corner (x,y)
        
        float re_x = face_row.at<float>(0, 4);
        float re_y = face_row.at<float>(0, 5);
        float le_x = face_row.at<float>(0, 6);
        float le_y = face_row.at<float>(0, 7);
        float nose_x = face_row.at<float>(0, 8);
        float nose_y = face_row.at<float>(0, 9);
        float rm_x = face_row.at<float>(0, 10);
        float rm_y = face_row.at<float>(0, 11);
        float lm_x = face_row.at<float>(0, 12);
        float lm_y = face_row.at<float>(0, 13);
        
        // 1. Translation normalization (Nose tip = origin)
        float p[5][2] = {
            {re_x - nose_x, re_y - nose_y},
            {le_x - nose_x, le_y - nose_y},
            {0.0f, 0.0f}, // Nose
            {rm_x - nose_x, rm_y - nose_y},
            {lm_x - nose_x, lm_y - nose_y}
        };
        
        // 2. Rotation & Scale normalization (align eyes horizontally and scale by eye distance)
        float dx = le_x - re_x;
        float dy = le_y - re_y;
        float eye_dist = std::sqrt(dx*dx + dy*dy);
        if (eye_dist <= 0) eye_dist = face_width * 0.4f; // Fallback
        
        float angle = std::atan2(dy, dx);
        float cos_a = std::cos(-angle);
        float sin_a = std::sin(-angle);
        
        std::vector<float> aligned;
        for (int i = 0; i < 5; ++i) {
            if (i == 2) continue; // Skip nose tip since its relative coordinates are always (0,0)
            
            // Rotate
            float rx = p[i][0] * cos_a - p[i][1] * sin_a;
            float ry = p[i][0] * sin_a + p[i][1] * cos_a;
            
            // Scale
            aligned.push_back(rx / eye_dist);
            aligned.push_back(ry / eye_dist);
        }
        
        history.push_back(aligned);
        if (history.size() > max_size) {
            history.erase(history.begin());
        }
    }

    void clear() {
        history.clear();
    }

    double calculateVariance() const {
        if (history.empty()) return 0.0;
        size_t num_coords = history[0].size();
        if (history.size() < 8) return 0.01; // Fail-safe during warm-up (assume moving)
        
        std::vector<double> means(num_coords, 0.0);
        for (const auto& frame : history) {
            for (size_t i = 0; i < num_coords; ++i) {
                means[i] += frame[i];
            }
        }
        for (size_t i = 0; i < num_coords; ++i) {
            means[i] /= history.size();
        }

        double total_variance = 0.0;
        for (const auto& frame : history) {
            for (size_t i = 0; i < num_coords; ++i) {
                double diff = frame[i] - means[i];
                total_variance += diff * diff;
            }
        }
        return total_variance / (history.size() * num_coords);
    }
};

// Passive Phone-ID Biometric Authentication Workflow
string runAuthentication(const string& user_feature_file) {
    Mat straight_feat, left_feat, right_feat;
    bool is_multi_angle = false;
    double optimal_crop_factor = 1.6;
    double optimal_threshold = 1.5;
    bool has_registered_face = loadFaceFeatures(straight_feat, left_feat, right_feat, is_multi_angle, optimal_crop_factor, optimal_threshold, user_feature_file);

    if (!has_registered_face) {
        cout << "[HATA] Sistemde kayitli yuz bulunamadi!" << endl;
        return "FAILURE";
    }

    // Dynamic configuration reload
    FaceAuthConfig config = readFaceAuthConfig();

    // Open Camera
    VideoCapture cap(0);
    if (!cap.isOpened()) {
        cout << "[HATA] Web kamerasi acilamadi!" << endl;
        return "FAILURE";
    }

    // Fast 3-frame validation queue for rock-solid passive liveness
    vector<float> liveness_history;
    const size_t STABILITY_WINDOW = 3;
    LandmarkHistory landmark_history;

    auto start_auth_time = chrono::high_resolution_clock::now();
    const double TIMEOUT_SEC = 6.5;

    Mat frame;
    bool auth_success = false;

    cout << "[INFO] Pasif Phone-ID taramasi baslatildi (Zaman Asimi: " << TIMEOUT_SEC << " sn)..." << endl;

    while (true) {
        // 1. Timeout Check
        auto current_time = chrono::high_resolution_clock::now();
        double elapsed_sec = chrono::duration_cast<chrono::microseconds>(current_time - start_auth_time).count() / 1000000.0;
        if (elapsed_sec >= TIMEOUT_SEC) {
            cout << "[UYARI] Zaman asimi! Pasif kimlik dogrulanamadi." << endl;
            break;
        }

        // 2. Read Frame
        cap >> frame;
        if (frame.empty()) {
            usleep(10000);
            continue;
        }

        flip(frame, frame, 1);

        Mat process_frame;
        resize(frame, process_frame, Size(320, 240));

        // CLAHE Contrast balancing
        if (config.enable_adaptive_exposure) {
            Mat lab_img;
            cvtColor(process_frame, lab_img, COLOR_BGR2Lab);
            vector<Mat> lab_planes(3);
            split(lab_img, lab_planes);
            Ptr<CLAHE> clahe = createCLAHE(2.0, Size(8, 8));
            clahe->apply(lab_planes[0], lab_planes[0]);
            merge(lab_planes, lab_img);
            cvtColor(lab_img, process_frame, COLOR_Lab2BGR);
        }

        detector->setInputSize(process_frame.size());

        // 3. Face Detection
        Mat faces;
        detector->detect(process_frame, faces);

        if (faces.rows > 0) {
            // Upscale coordinates
            float scale_x = static_cast<float>(frame.cols) / static_cast<float>(process_frame.cols);
            float scale_y = static_cast<float>(frame.rows) / static_cast<float>(process_frame.rows);

            int ux = static_cast<int>(faces.at<float>(0, 0) * scale_x);
            int uy = static_cast<int>(faces.at<float>(0, 1) * scale_y);
            int uw = static_cast<int>(faces.at<float>(0, 2) * scale_x);
            int uh = static_cast<int>(faces.at<float>(0, 3) * scale_y);
            Rect bbox(ux, uy, uw, uh);

            double bbox_expansion_factor = optimal_crop_factor;
            int max_dim = max(bbox.width, bbox.height);
            double center_x = bbox.x + bbox.width / 2.0;
            double center_y = bbox.y + bbox.height / 2.0;

            int nx = static_cast<int>(center_x - max_dim * bbox_expansion_factor / 2.0);
            int ny = static_cast<int>(center_y - max_dim * bbox_expansion_factor / 2.0);
            int crop_size = static_cast<int>(max_dim * bbox_expansion_factor);

            int crop_x1 = max(0, nx);
            int crop_y1 = max(0, ny);
            int crop_x2 = min(frame.cols, nx + crop_size);
            int crop_y2 = min(frame.rows, ny + crop_size);

            int top_pad = max(0, -ny);
            int left_pad = max(0, -nx);
            int bottom_pad = max(0, (ny + crop_size) - frame.rows);
            int right_pad = max(0, (nx + crop_size) - frame.cols);

            Mat cropped_face;
            if (crop_x2 > crop_x1 && crop_y2 > crop_y1) {
                cropped_face = frame(Range(crop_y1, crop_y2), Range(crop_x1, crop_x2));
            } else {
                cropped_face = Mat::zeros(0, 0, frame.type());
            }

            Mat padded_crop;
            copyMakeBorder(cropped_face, padded_crop, top_pad, bottom_pad, left_pad, right_pad, BORDER_REFLECT_101);

            Mat gray_face;
            cvtColor(padded_crop, gray_face, COLOR_BGR2GRAY);
            Scalar mean_val = mean(gray_face);
            double avg_brightness = mean_val[0];

            if (avg_brightness < config.min_brightness) {
                cout << "[UYARI] Ortalama parlaklik dusuk: " << avg_brightness << endl;
            }

            // 4. Passive Liveness Check (MiniFASNet AI or Texture filter)
            bool is_live = true;
            string liveness_reason = "";

            if (config.liveness_method == "minifas" && !liveness_net.empty()) {
                Mat resized_face;
                resize(padded_crop, resized_face, Size(128, 128), 0, 0, INTER_AREA);

                Mat inputBlob = dnn::blobFromImage(resized_face, 1.0 / 255.0, Size(128, 128), Scalar(0, 0, 0), true, false);
                liveness_net.setInput(inputBlob);
                Mat liveness_output = liveness_net.forward();
     
                float real_logit = liveness_output.at<float>(0, 0); 
                float spoof_logit = liveness_output.at<float>(0, 1); 
                float logit_diff = real_logit - spoof_logit;

                liveness_history.push_back(logit_diff);
                if (liveness_history.size() > STABILITY_WINDOW) {
                    liveness_history.erase(liveness_history.begin());
                }

                // Verify liveness over 3 consecutive frames for zero-friction stability
                int stable_live_count = 0;
                for (float val : liveness_history) {
                    if (val >= optimal_threshold) {
                        stable_live_count++;
                    }
                }
                
                // Must have full stability window filled and all values above threshold
                is_live = (liveness_history.size() >= STABILITY_WINDOW && stable_live_count == STABILITY_WINDOW);
                
                // Add landmarks for micro-movement physiological jitter check
                landmark_history.add(faces.row(0), uw);
                double landmark_variance = landmark_history.calculateVariance();
                bool micro_movement_detected = (landmark_variance >= 1.5e-5 && landmark_variance <= 8.0e-4);

                // --- HİBRİT GÜVENLİK OVERRIDE ---
                if (!is_live && micro_movement_detected && logit_diff >= -4.0f) {
                    is_live = true;
                }

                if (!is_live) {
                    liveness_reason = "Yapay Zeka Canlilik Analizi Bekleniyor (Diff: " + to_string(logit_diff) + ", Var: " + to_string(landmark_variance) + ")";
                }

                cout << "[LIVENESS] Yapay Zeka Pasif | Diff: " << logit_diff << " | Var: " << landmark_variance << " | Canli mi: " << (is_live ? "EVET" : "BEKLEMEDE") << endl;

            } else if (config.liveness_method == "texture") {
                Mat laplacian;
                Laplacian(gray_face, laplacian, CV_64F);
                Scalar mean_lap, stddev_lap;
                meanStdDev(laplacian, mean_lap, stddev_lap);
                double laplacian_var = stddev_lap[0] * stddev_lap[0];

                Mat hsv;
                cvtColor(padded_crop, hsv, COLOR_BGR2HSV);
                vector<Mat> hsv_channels;
                split(hsv, hsv_channels);
                Scalar mean_sat = mean(hsv_channels[1]);
                double hsv_sat_mean = mean_sat[0];

                if (laplacian_var < config.texture_min_laplacian && hsv_sat_mean < config.texture_min_hsv_sat) {
                    is_live = false;
                    liveness_reason = "Bloke: Kagit Fotograf / Dusuk Renk Doygunlugu";
                } else if (laplacian_var > config.texture_max_laplacian) {
                    is_live = false;
                    liveness_reason = "Bloke: Dijital Ekran / Moire Deseni";
                } else if (hsv_sat_mean > config.texture_max_hsv_sat) {
                    is_live = false;
                    liveness_reason = "Bloke: Yapay Ekran Işıltısı";
                }

                cout << "[LIVENESS] Doku Pasif | Lap Var: " << laplacian_var << " | Sat: " << hsv_sat_mean << " | Canli: " << (is_live ? "EVET" : "HAYIR") << endl;
            }

            // 5. Face Recognition comparison (only runs if liveness is verified)
            if (is_live) {
                Mat aligned_face;
                recognizer->alignCrop(process_frame, faces.row(0), aligned_face);

                Mat face_feature;
                recognizer->feature(aligned_face, face_feature);

                double score_straight = recognizer->match(straight_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                double max_score = score_straight;
                string matched_angle = "DUZ";

                if (is_multi_angle) {
                    double score_left = recognizer->match(left_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                    double score_right = recognizer->match(right_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                    if (score_left > max_score) {
                        max_score = score_left;
                        matched_angle = "SOL";
                    }
                    if (score_right > max_score) {
                        max_score = score_right;
                        matched_angle = "SAG";
                    }
                }

                bool is_match = (max_score >= config.matching_threshold);

                if (is_match) {
                    cout << "[OK] Eslesme Basarili! Aci: " << matched_angle << " | Benzerlik Puanı: " << max_score << endl;
                    auth_success = true;
                    break;
                } else {
                    cout << "[BILGI] Yuz algilandi ama eslesmedi. En iyi benzerlik: " << max_score << endl;
                }
            } else {
                if (!liveness_reason.empty()) {
                    cout << "[UYARI] Pasif canlilik engeli: " << liveness_reason << endl;
                }
            }
        } else {
            liveness_history.clear();
            landmark_history.clear();
        }

        // Dynamic core throttling for extreme low CPU usage
        if (faces.rows > 0) {
            usleep(10000);  // 10ms dynamic sleep during active face processing
        } else {
            usleep(40000);  // 40ms sleep when idle
        }
    }

    cap.release();
    return auth_success ? "SUCCESS" : "FAILURE";
}

int main() {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    cout << "==================================================" << endl;
    cout << "  FaceAuth NextGen - Sıfırdan Pasif FaceID Servisi " << endl;
    cout << "==================================================" << endl;
    cout << "[INFO] Modeller yukleniyor..." << endl;

    string det_path = get_model_path("yunet.onnx");
    string rec_path = get_model_path("sface.onnx");
    string live_path = get_model_path("minifas.onnx");

    detector = FaceDetectorYN::create(det_path, "", Size(320, 240), 0.6f, 0.3f, 5000);
    recognizer = FaceRecognizerSF::create(rec_path, "");

    if (detector.empty() || recognizer.empty()) {
        cerr << "[HATA] Dedektör veya yüz tanıma modeli yüklenemedi!" << endl;
        return -1;
    }

    if (file_exists(live_path)) {
        try {
            liveness_net = dnn::readNetFromONNX(live_path);
            if (!liveness_net.empty()) {
                liveness_net.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
                liveness_net.setPreferableTarget(dnn::DNN_TARGET_CPU);
                cout << "[OK] MiniFASNet anti-spoofing yapay zeka modeli yuklendi." << endl;
            }
        } catch (...) {
            cout << "[UYARI] MiniFASNet modeli yuklenemedi! Pasif yapay zeka canliligi aktif olmayacaktir." << endl;
        }
    } else {
        cout << "[UYARI] MiniFASNet model dosyasi bulunamadi!" << endl;
    }

    // Dynamic CPU threading optimization (Phone-Like core control)
    int cpu_cores = cv::getNumberOfCPUs();
    int optimal_threads = max(1, min(4, cpu_cores / 2));
    setNumThreads(optimal_threads);
    cout << "[OK] CPU Is Parcacigi Limiti: " << optimal_threads << " / " << cpu_cores << endl;

    // Set up UNIX socket for PAM
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        cerr << "[HATA] Soket olusturulamadi!" << endl;
        return -1;
    }

    sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH.c_str(), sizeof(server_addr.sun_path) - 1);

    unlink(SOCKET_PATH.c_str());

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        cerr << "[HATA] Soket baglanamadi (bind)!" << endl;
        return -1;
    }

    // Ensure secure socket permissions (read/write access for faceauth group)
    chmod(SOCKET_PATH.c_str(), 0660);
    chown(SOCKET_PATH.c_str(), 0, getgrnam("faceauth") ? getgrnam("faceauth")->gr_gid : 0);

    if (listen(server_fd, 5) == -1) {
        cerr << "[HATA] Dinleme baslatilamadi!" << endl;
        return -1;
    }

    cout << "[OK] FaceAuth servisi hazir. UNIX soket dinleniyor..." << endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd == -1) continue;

        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            string payload = trim(string(buffer));
            string username = payload;
            if (payload.rfind("AUTH_REQUEST:", 0) == 0) {
                username = payload.substr(13);
            }
            username = trim(username);
            cout << "[DAEMON] Yetkilendirme istegi alindi. Kullanici: " << username << endl;

            string user_feature_file = "/var/lib/faceauth/users/" + username + ".yml";
            if (!file_exists(user_feature_file)) {
                user_feature_file = "models/" + username + "_face.yml";
            }

            string result = runAuthentication(user_feature_file);
            cout << "[DAEMON] Yetkilendirme sonucu: " << result << endl;

            write(client_fd, result.c_str(), result.length());
        }

        close(client_fd);
    }

    close(server_fd);
    unlink(SOCKET_PATH.c_str());
    return 0;
}
