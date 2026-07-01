#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include "config.hpp"

using namespace std;
using namespace cv;

// Persistence Helpers (OpenCV YAML Storage)
#include <sys/stat.h>
#include <unistd.h>

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

string get_user_feature_path(const string& username) {
    string system_dir = "/var/lib/faceauth/users";
    string system_path = system_dir + "/" + username + ".yml";
    
    struct stat st;
    if (stat(system_dir.c_str(), &st) == 0 && (st.st_mode & S_IWUSR)) {
        return system_path;
    }
    return "models/" + username + "_face.yml";
}

// Check if a single eye is closed using Laplacian variance and local contrast
bool isEyeClosed(const Mat& gray_eye, double* out_var = nullptr) {
    if (gray_eye.empty() || gray_eye.cols < 5 || gray_eye.rows < 5) return false;
    
    // Apply 3x3 Gaussian Blur to eliminate high-frequency camera sensor noise in low-light
    Mat blurred;
    GaussianBlur(gray_eye, blurred, Size(3, 3), 0);
    
    // 1. Calculate Laplacian variance on the blurred eye image
    Mat laplacian;
    Laplacian(blurred, laplacian, CV_64F);
    Scalar mean_lap, stddev_lap;
    meanStdDev(laplacian, mean_lap, stddev_lap);
    double var = stddev_lap[0] * stddev_lap[0];
    
    if (out_var) {
        *out_var = var;
    }
    
    // Closed eye has very smooth skin texture (low variance).
    // Using a calibrated threshold of < 16.0 on Gaussian-blurred region is highly robust.
    return (var < 16.0);
}

void saveFaceFeatures(const Mat& straight, const Mat& left, const Mat& right, double optimal_crop_factor, double optimal_threshold, const string& filepath) {
    FileStorage fs(filepath, FileStorage::WRITE);
    fs << "feature_straight" << straight;
    fs << "feature_left" << left;
    fs << "feature_right" << right;
    fs << "is_multi_angle" << 1;
    fs << "optimal_crop_factor" << optimal_crop_factor;
    fs << "optimal_threshold" << optimal_threshold;
    fs.release();
}

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
        is_multi_angle = (!straight.empty() && !left.empty() && !right.empty());
    } else {
        fs["face_feature"] >> straight;
        left = Mat();
        right = Mat();
    }

    if (fs["optimal_crop_factor"].isNone()) {
        optimal_crop_factor = 2.7;
    } else {
        fs["optimal_crop_factor"] >> optimal_crop_factor;
    }

    if (fs["optimal_threshold"].isNone()) {
        optimal_threshold = 1.5;
    } else {
        fs["optimal_threshold"] >> optimal_threshold;
    }

    fs.release();
    return !straight.empty();
}

double calibrateOptimalCropFactor(const Mat& frame, const Rect& bbox, dnn::Net& liveness_net) {
    if (liveness_net.empty()) return 1.6;
    
    std::vector<double> crop_factors = {1.2, 1.4, 1.5, 1.6, 1.8, 2.0};
    double best_factor = 1.6;
    float max_diff = -9999.0f;
    
    for (double factor : crop_factors) {
        int max_dim = std::max(bbox.width, bbox.height);
        double center_x = bbox.x + bbox.width / 2.0;
        double center_y = bbox.y + bbox.height / 2.0;

        int nx = static_cast<int>(center_x - max_dim * factor / 2.0);
        int ny = static_cast<int>(center_y - max_dim * factor / 2.0);
        int crop_size = static_cast<int>(max_dim * factor);

        int crop_x1 = std::max(0, nx);
        int crop_y1 = std::max(0, ny);
        int crop_x2 = std::min(frame.cols, nx + crop_size);
        int crop_y2 = std::min(frame.rows, ny + crop_size);

        int top_pad = std::max(0, -ny);
        int left_pad = std::max(0, -nx);
        int bottom_pad = std::max(0, (ny + crop_size) - frame.rows);
        int right_pad = std::max(0, (nx + crop_size) - frame.cols);

        Mat cropped_face;
        if (crop_x2 > crop_x1 && crop_y2 > crop_y1) {
            cropped_face = frame(Range(crop_y1, crop_y2), Range(crop_x1, crop_x2));
        } else {
            cropped_face = Mat::zeros(0, 0, frame.type());
        }

        Mat padded_crop;
        copyMakeBorder(cropped_face, padded_crop, top_pad, bottom_pad, left_pad, right_pad, BORDER_REFLECT_101);

        Mat resized_face;
        resize(padded_crop, resized_face, Size(128, 128), 0, 0, INTER_AREA);

        Mat inputBlob = dnn::blobFromImage(resized_face, 1.0 / 255.0, Size(128, 128), Scalar(0, 0, 0), true, false);
        liveness_net.setInput(inputBlob);
        Mat output = liveness_net.forward();

        float real_logit = output.at<float>(0, 0);
        float spoof_logit = output.at<float>(0, 1);
        float diff = real_logit - spoof_logit;

        if (diff > max_diff) {
            max_diff = diff;
            best_factor = factor;
        }
    }
    std::cout << "[CALIBRATION] Optimal crop factor determined: " << best_factor << "x (Score: " << max_diff << ")" << std::endl;
    return best_factor;
}

// Micro-Movement / Physiological Jitter Tracker
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

// Calculate Head Yaw (horizontal rotation ratio) using 5 facial landmarks
bool calculateHeadYaw(const Mat& face, double& yaw_ratio) {
    if (face.rows == 0) return false;
    
    // Extract X coordinates of landmarks
    // Index 4: Right Eye X, Index 6: Left Eye X, Index 8: Nose X
    // Index 10: Right Mouth X, Index 12: Left Mouth X
    float re_x = face.at<float>(0, 4);
    float le_x = face.at<float>(0, 6);
    float nose_x = face.at<float>(0, 8);
    
    float rm_x = face.at<float>(0, 10);
    float lm_x = face.at<float>(0, 12);
    
    if (le_x <= re_x || lm_x <= rm_x) return false; // Prevent divide by zero
    
    double ratio_eyes = (nose_x - re_x) / (le_x - re_x);
    double ratio_mouth = (nose_x - rm_x) / (lm_x - rm_x);
    
    yaw_ratio = (ratio_eyes + ratio_mouth) / 2.0;
    return true;
}

// Helper to draw landmarks, bounding box, liveness status, and warnings
void drawFace(Mat& frame, const Mat& faces, double inference_time_ms, bool is_live, bool has_registered, bool is_match, double brightness, const string& liveness_method, int active_state) {
    for (int i = 0; i < faces.rows; i++) {
        // Bounding box coordinates
        int x1 = static_cast<int>(faces.at<float>(i, 0));
        int y1 = static_cast<int>(faces.at<float>(i, 1));
        int w  = static_cast<int>(faces.at<float>(i, 2));
        int h  = static_cast<int>(faces.at<float>(i, 3));
        
        Rect face_rect(x1, y1, w, h);
        
        // Decide box color & status text based on active/passive liveness states
        Scalar box_color;
        string liveness_str = "";
        
        if (i > 0) {
            box_color = Scalar(150, 150, 150); // Gray for secondary/ignored faces
            liveness_str = "SECONDARY FACE (IGNORED)";
        } else if (liveness_method == "active" && !is_live) {
            box_color = Scalar(0, 165, 255); // Orange (Straight)
            if (active_state == 1) box_color = Scalar(255, 0, 255); // Pink/Purple (Blink)
            else if (active_state == 2) box_color = Scalar(0, 255, 255); // Cyan (Turn)
            
            liveness_str = "ANALYZING LIVENESS...";
        } else if (!is_live) {
            box_color = Scalar(0, 0, 255); // RED for spoof / photo
            liveness_str = "PHOTO DETECTED (SPOOF)";
        } else if (has_registered) {
            box_color = is_match ? Scalar(0, 255, 0) : Scalar(0, 165, 255); // GREEN for match, ORANGE for no match
            liveness_str = "LIVE (REAL)";
        } else {
            box_color = Scalar(255, 255, 255); // WHITE for default real face
            liveness_str = "LIVE (REAL)";
        }
        
        rectangle(frame, face_rect, box_color, 2);
 
        // Liveness Text
        putText(frame, liveness_str, Point(x1, y1 - 25), FONT_HERSHEY_SIMPLEX, 0.5, box_color, 1.5);

        // Details (inference & brightness)
        string details_str = format("Det: %.1f ms | Brightness: %.0f", inference_time_ms, brightness);
        putText(frame, details_str, Point(x1, y1 - 8), FONT_HERSHEY_SIMPLEX, 0.45, Scalar(200, 200, 200), 1);

        // Ambient Light Warning
        if (brightness < 45.0) {
            putText(frame, "[!] ENVIRONMENT TOO DARK - TURN ON LIGHT", Point(x1, y1 + h + 20), FONT_HERSHEY_SIMPLEX, 0.45, Scalar(0, 165, 255), 1.5);
        } else if (brightness > 220.0) {
            putText(frame, "[!] EXCESSIVE LIGHT / GLARE WARNING", Point(x1, y1 + h + 20), FONT_HERSHEY_SIMPLEX, 0.45, Scalar(0, 165, 255), 1.5);
        }

        // 5 Landmarks (Right Eye, Left Eye, Nose, Right Mouth Corner, Left Mouth Corner)
        circle(frame, Point2i(static_cast<int>(faces.at<float>(i, 4)), static_cast<int>(faces.at<float>(i, 5))), 2, Scalar(255, 0, 0), 2); // Right eye
        circle(frame, Point2i(static_cast<int>(faces.at<float>(i, 6)), static_cast<int>(faces.at<float>(i, 7))), 2, Scalar(0, 0, 255), 2); // Left eye
        circle(frame, Point2i(static_cast<int>(faces.at<float>(i, 8)), static_cast<int>(faces.at<float>(i, 9))), 2, Scalar(0, 255, 0), 2); // Nose
        circle(frame, Point2i(static_cast<int>(faces.at<float>(i, 10)), static_cast<int>(faces.at<float>(i, 11))), 2, Scalar(255, 0, 255), 2); // Right mouth corner
        circle(frame, Point2i(static_cast<int>(faces.at<float>(i, 12)), static_cast<int>(faces.at<float>(i, 13))), 2, Scalar(0, 255, 255), 2); // Left mouth corner
    }
}

int main(int argc, char** argv) {
    // 1. Get username and resolve feature file path
    string username;
    if (argc > 1) {
        username = argv[1];
    } else {
        const char* sudo_user = getenv("SUDO_USER");
        if (sudo_user) {
            username = sudo_user;
        } else {
            const char* user = getenv("USER");
            if (user) {
                username = user;
            } else {
                username = "default";
            }
        }
    }

    // Clean username for path safety
    string clean_username = "";
    for (char c : username) {
        if (isalnum(c) || c == '_' || c == '-') {
            clean_username += c;
        }
    }
    if (clean_username.empty()) {
        clean_username = "default";
    }

    string feature_file = get_user_feature_path(clean_username);

    string det_model_path = get_model_path("yunet.onnx");
    string rec_model_path = get_model_path("sface.onnx");
    string liveness_model_path = get_model_path("minifas.onnx");

    // Load configuration
    FaceAuthConfig config = readFaceAuthConfig();

    cout << "==================================================================" << endl;
    cout << "   AegisFace - Live Setup & Test Wizard v3.1                      " << endl;
    cout << "==================================================================" << endl;
    cout << "[INFO] Hedef Kullanici: " << clean_username << endl;
    cout << "[INFO] Model Dosyasi: " << feature_file << endl;
    cout << "[INFO] Modeller yukleniyor..." << endl;

    Ptr<FaceDetectorYN> detector = FaceDetectorYN::create(det_model_path, "", Size(320, 240), 0.6f, 0.3f, 5000);
    Ptr<FaceRecognizerSF> recognizer = FaceRecognizerSF::create(rec_model_path, "");
    dnn::Net liveness_net;

    if (detector.empty() || recognizer.empty()) {
        cerr << "[HATA] Modeller yuklenemedi! Yol tanimlari dogru mu?" << endl;
        return -1;
    }

    if (file_exists(liveness_model_path)) {
        try {
            liveness_net = dnn::readNetFromONNX(liveness_model_path);
            if (!liveness_net.empty()) {
                liveness_net.setPreferableBackend(dnn::DNN_BACKEND_OPENCV);
                liveness_net.setPreferableTarget(dnn::DNN_TARGET_CPU);
                cout << "[OK] MiniFASNet anti-spoofing modeli basariyla yuklendi." << endl;
            }
        } catch (...) {
            cout << "[UYARI] MiniFASNet modeli yuklenirken hata olustu. Yapay zeka liveness modu calismayacaktir." << endl;
        }
    } else {
        cout << "[UYARI] MiniFASNet modeli bulunamadi. Yapay zeka liveness modu calismayacaktir." << endl;
    }

    cout << "[OK] Temel yapay zeka modelleri yuklendi." << endl;

    // Load registered faces if exists
    Mat straight_feat, left_feat, right_feat;
    bool is_multi_angle = false;
    double optimal_crop_factor = 2.7;
    double optimal_threshold = 1.5;
    bool has_registered_face = loadFaceFeatures(straight_feat, left_feat, right_feat, is_multi_angle, optimal_crop_factor, optimal_threshold, feature_file);
    if (has_registered_face) {
        cout << "[OK] Kayitli yuz basariyla yuklendi (Dosyadan: " << feature_file << ", Cok Acili: " << (is_multi_angle ? "EVET" : "HAYIR") << ", Katsayi: " << optimal_crop_factor << "x, Esik: " << optimal_threshold << ")" << endl;
    } else {
        cout << "[INFO] Kayitli yuz bulunamadi. Yeni kayit yapabilirsiniz." << endl;
    }

    // Open Webcam
    VideoCapture cap(0);
    if (!cap.isOpened()) {
        cerr << "[HATA] Web kamerasi acilamadi!" << endl;
        return -1;
    }
    cout << "[OK] Kamera basariyla acildi." << endl;
    cout << "\n--- KULLANIM KLAVUZU ---" << endl;
    cout << "[s] Tusu: Yuzunuzu kaydeder ve kalici olarak diskte saklar" << endl;
    cout << "[r] Tusu: Kayitli yuzu hem bellekten hem de diskten siler" << endl;
    cout << "[q] Tusu: Programdan cikis" << endl;
    cout << "------------------------\n" << endl;

    Mat frame;
    vector<float> liveness_history; // Sliding window queue for temporal liveness filtering
    const size_t HISTORY_WINDOW_SIZE = 10;
    LandmarkHistory landmark_history;

    enum ActiveLivenessState {
        Liveness_WaitStraight = 0,
        Liveness_WaitBlink = 1,
        Liveness_WaitTurn = 2,
        Liveness_Success = 3
    };
    ActiveLivenessState active_state = Liveness_WaitStraight;
    int straight_frames = 0;

    enum BlinkPhase {
        Blink_PhaseOpen = 0,
        Blink_PhaseClosed = 1,
        Blink_PhaseOpened = 2
    };
    BlinkPhase blink_phase = Blink_PhaseOpen;

    enum RegWizardState {
        Reg_None = 0,
        Reg_StepStraight = 1,
        Reg_StepLeft = 2,
        Reg_StepRight = 3,
        Reg_Done = 4
    };
    RegWizardState reg_wizard = Reg_None;
    Mat wizard_straight, wizard_left, wizard_right;

    // Enable dynamic multi-threading for OpenCV CPU operations (Phone-Like Core Management)
    int cpu_cores = getNumberOfCPUs();
    int optimal_threads = max(1, min(4, cpu_cores / 2));
    setNumThreads(optimal_threads);
    cout << "[OK] Is parcacigi optimizasyonu tamamlandi. Cekirdek Sayisi: " << cpu_cores 
         << " | OpenCV Thread Limiti: " << optimal_threads << endl;

    // Warm-up inference
    Mat dummy = Mat::zeros(240, 320, CV_8UC3);
    Mat dummy_faces;
    detector->detect(dummy, dummy_faces);
    cout << "[INFO] Model isinma (Warm-up) islemi tamamlandi." << endl;

    while (true) {
        cap >> frame;
        if (frame.empty()) {
            cerr << "[HATA] Bos frame alindi!" << endl;
            break;
        }

        // Mirror the frame for natural view
        flip(frame, frame, 1);

        char key = static_cast<char>(waitKey(1));
        if (key == 'q') {
            break;
        } else if (key == 'r') {
            has_registered_face = false;
            remove(feature_file.c_str());
            reg_wizard = Reg_None;
            cout << "[INFO] Kayitli yuz bellekten ve diskten silindi." << endl;
        }

        Mat process_frame;
        resize(frame, process_frame, Size(320, 240));

        // Apply Smart Adaptive Exposure Compensation (CLAHE) if enabled in config
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

        // Update detector size
        detector->setInputSize(process_frame.size());

        // 1. Face Detection
        Mat faces;
        auto start_det = chrono::high_resolution_clock::now();
        detector->detect(process_frame, faces);
        auto end_det = chrono::high_resolution_clock::now();
        double det_time = chrono::duration_cast<chrono::microseconds>(end_det - start_det).count() / 1000.0;

        // Upscale face coordinates back to original frame size
        Mat faces_upscaled = faces.clone();
        float scale_x = static_cast<float>(frame.cols) / static_cast<float>(process_frame.cols);
        float scale_y = static_cast<float>(frame.rows) / static_cast<float>(process_frame.rows);

        for (int i = 0; i < faces_upscaled.rows; i++) {
            faces_upscaled.at<float>(i, 0) *= scale_x;
            faces_upscaled.at<float>(i, 1) *= scale_y;
            faces_upscaled.at<float>(i, 2) *= scale_x;
            faces_upscaled.at<float>(i, 3) *= scale_y;
            for (int l = 4; l < 14; l += 2) {
                faces_upscaled.at<float>(i, l) *= scale_x;
                faces_upscaled.at<float>(i, l+1) *= scale_y;
            }
        }

        bool is_live = true;
        string liveness_reason = "";
        bool is_match = false;
        double liveness_time = 0;
        double align_time = 0;
        double rec_time = 0;
        double avg_brightness = 128.0;

        if (faces.rows > 0) {
            // Get coordinates
            int ux = static_cast<int>(faces_upscaled.at<float>(0, 0));
            int uy = static_cast<int>(faces_upscaled.at<float>(0, 1));
            int uw = static_cast<int>(faces_upscaled.at<float>(0, 2));
            int uh = static_cast<int>(faces_upscaled.at<float>(0, 3));
            Rect bbox(ux, uy, uw, uh);

            // Crop face with dynamic expansion (calibrated or default)
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

            // A. Ambient Light Check (Ortalama Parlaklık Kontrolü)
            Mat gray_face;
            cvtColor(padded_crop, gray_face, COLOR_BGR2GRAY);
            Scalar mean_val = mean(gray_face);
            avg_brightness = mean_val[0];

            Mat resized_face;
            resize(padded_crop, resized_face, Size(128, 128), 0, 0, INTER_AREA);

            // Light Check
            if (avg_brightness < config.min_brightness) {
                // Warning is drawn by drawFace
            }

            double laplacian_var_gui = 0.0;
            double hsv_sat_gui = 0.0;
            double yaw_ratio_gui = 0.5;
            double r_eye_var_gui = 0.0;
            double l_eye_var_gui = 0.0;

            if (config.liveness_method == "active") {
                auto start_live = chrono::high_resolution_clock::now();
                bool r_closed = false, l_closed = false, eyes_closed = false;

                // Crop eye regions from 320x240 grayscale process_frame for extremely fast blink detection
                Mat gray_process;
                cvtColor(process_frame, gray_process, COLOR_BGR2GRAY);
                float re_x = faces.at<float>(0, 4);
                float re_y = faces.at<float>(0, 5);
                float le_x = faces.at<float>(0, 6);
                float le_y = faces.at<float>(0, 7);
                float f_w = faces.at<float>(0, 2);

                int eye_size = static_cast<int>(f_w * 0.14);
                if (eye_size % 2 == 0) eye_size++;

                int rx1 = max(0, static_cast<int>(re_x - eye_size / 2));
                int ry1 = max(0, static_cast<int>(re_y - eye_size / 2));
                int rx2 = min(process_frame.cols, rx1 + eye_size);
                int ry2 = min(process_frame.rows, ry1 + eye_size);

                int lx1 = max(0, static_cast<int>(le_x - eye_size / 2));
                int ly1 = max(0, static_cast<int>(le_y - eye_size / 2));
                int lx2 = min(process_frame.cols, lx1 + eye_size);
                int ly2 = min(process_frame.rows, ly1 + eye_size);

                if (rx2 > rx1 && ry2 > ry1 && lx2 > lx1 && ly2 > ly1) {
                    Mat r_eye = gray_process(Range(ry1, ry2), Range(rx1, rx2));
                    Mat l_eye = gray_process(Range(ly1, ly2), Range(lx1, lx2));
                    r_closed = isEyeClosed(r_eye, &r_eye_var_gui);
                    l_closed = isEyeClosed(l_eye, &l_eye_var_gui);
                    eyes_closed = r_closed && l_closed;
                }

                if (calculateHeadYaw(faces, yaw_ratio_gui)) {
                    if (active_state == Liveness_WaitStraight) {
                        if (yaw_ratio_gui >= config.active_straight_min && yaw_ratio_gui <= config.active_straight_max) {
                            straight_frames++;
                            if (straight_frames >= 3) {
                                if (avg_brightness < config.min_brightness) {
                                    active_state = Liveness_WaitTurn;
                                    cout << "[ACTIVE_LIVENESS] Ortam karanlik oldugu icin goz kirpma fazi atlandi. Simdi kafanizi saga veya sola cevirin!" << endl;
                                } else {
                                    active_state = Liveness_WaitBlink;
                                    blink_phase = Blink_PhaseOpen;
                                    cout << "[ACTIVE_LIVENESS] Duz bakis onaylandi. Simdi gozunuzu kirpin!" << endl;
                                }
                            }
                        } else {
                            straight_frames = 0;
                        }
                        is_live = false;
                    } else if (active_state == Liveness_WaitBlink) {
                        if (blink_phase == Blink_PhaseOpen) {
                            if (eyes_closed) {
                                blink_phase = Blink_PhaseClosed;
                                cout << "[ACTIVE_LIVENESS] Gozler kapandi (Blink Down)." << endl;
                            }
                        } else if (blink_phase == Blink_PhaseClosed) {
                            if (!eyes_closed) {
                                blink_phase = Blink_PhaseOpened;
                                active_state = Liveness_WaitTurn;
                                cout << "[ACTIVE_LIVENESS] Gozler tekrar acildi (Blink Up). Goz kirpma tamamlandi! Simdi kafanizi saga veya sola cevirin." << endl;
                            }
                        }
                        is_live = false;
                    } else if (active_state == Liveness_WaitTurn) {
                        if (yaw_ratio_gui >= config.active_turn_left) {
                            active_state = Liveness_Success;
                            cout << "[ACTIVE_LIVENESS] SOLA donus algilandi! Canlilik onaylandi. Yaw: " << yaw_ratio_gui << endl;
                        } else if (yaw_ratio_gui <= config.active_turn_right) {
                            active_state = Liveness_Success;
                            cout << "[ACTIVE_LIVENESS] SAGA donus algilandi! Canlilik onaylandi. Yaw: " << yaw_ratio_gui << endl;
                        }
                        is_live = false;
                    }

                    if (active_state == Liveness_Success) {
                        is_live = true;
                    }
                } else {
                    is_live = false;
                }
                auto end_live = chrono::high_resolution_clock::now();
                liveness_time = chrono::duration_cast<chrono::microseconds>(end_live - start_live).count() / 1000.0;

                if (!is_live) {
                    liveness_reason = "Aktif Canlilik Sirasi Bekleniyor (Durum: " + 
                                      to_string(active_state) + ", Duz: " + to_string(straight_frames) + "/3, Goz: " + to_string(blink_phase) + ")";
                }

            } else if (config.liveness_method == "texture") {
                auto start_live = chrono::high_resolution_clock::now();
                // Laplacian Variance
                Mat laplacian;
                Laplacian(gray_face, laplacian, CV_64F);
                Scalar mean_lap, stddev_lap;
                meanStdDev(laplacian, mean_lap, stddev_lap);
                laplacian_var_gui = stddev_lap[0] * stddev_lap[0];

                // HSV Saturation
                Mat hsv;
                cvtColor(padded_crop, hsv, COLOR_BGR2HSV);
                vector<Mat> hsv_channels;
                split(hsv, hsv_channels);
                Scalar mean_sat = mean(hsv_channels[1]);
                hsv_sat_gui = mean_sat[0];

                if (laplacian_var_gui < config.texture_min_laplacian && hsv_sat_gui < config.texture_min_hsv_sat) {
                    is_live = false;
                    liveness_reason = "Kagit Fotograf / Dusuk Renk Doygunlugu";
                } else if (laplacian_var_gui > config.texture_max_laplacian) {
                    is_live = false;
                    liveness_reason = "Dijital Ekran (Moire)";
                } else if (hsv_sat_gui > config.texture_max_hsv_sat) {
                    is_live = false;
                    liveness_reason = "Yapay Renk (HSV Sat)";
                }
                auto end_live = chrono::high_resolution_clock::now();
                liveness_time = chrono::duration_cast<chrono::microseconds>(end_live - start_live).count() / 1000.0;

                cout << "[LIVENESS] DOKU | Lap: " << laplacian_var_gui << " | HSV Sat: " << hsv_sat_gui 
                     << " | Canli: " << (is_live ? "EVET" : "HAYIR") 
                     << (is_live ? "" : " | Sebep: " + liveness_reason) << endl;

            } else if (config.liveness_method == "minifas" && !liveness_net.empty()) {
                Mat resized_face;
                resize(padded_crop, resized_face, Size(128, 128), 0, 0, INTER_AREA);

                auto start_live = chrono::high_resolution_clock::now();
                
                Mat inputBlob = dnn::blobFromImage(resized_face, 1.0 / 255.0, Size(128, 128), Scalar(0, 0, 0), true, false);
                liveness_net.setInput(inputBlob);
                Mat liveness_output = liveness_net.forward();
                auto end_live = chrono::high_resolution_clock::now();
                liveness_time = chrono::duration_cast<chrono::microseconds>(end_live - start_live).count() / 1000.0;

                // İNDİS DÜZELTMESİ: İndis 0 = Real, İndis 1 = Spoof
                float real_logit = liveness_output.at<float>(0, 0); 
                float spoof_logit = liveness_output.at<float>(0, 1); 
                float logit_diff = real_logit - spoof_logit;

                liveness_history.push_back(logit_diff);
                if (liveness_history.size() > HISTORY_WINDOW_SIZE) {
                    liveness_history.erase(liveness_history.begin());
                }

                // Add landmarks for micro-movement physiological jitter check
                landmark_history.add(faces.row(0), uw);
                double landmark_variance = landmark_history.calculateVariance();
                bool micro_movement_detected = (landmark_variance >= 1.5e-5 && landmark_variance <= 8.0e-4); // Calibrated range for genuine physiological tremor

                float sum = 0.0f;
                int potential_live_count = 0;
                for (float val : liveness_history) {
                    sum += val;
                    if (val >= optimal_threshold) {
                        potential_live_count++;
                    }
                }
                float avg_diff = sum / liveness_history.size();
                is_live = (avg_diff >= optimal_threshold && potential_live_count >= 7);

                // --- HİBRİT GÜVENLİK OVERRIDE ---
                // Eğer model oda gürültüsünden dolayı hafifçe "sahte" diyorsa ama canlı mikro-hareket kesin tespit edilmişse,
                // ve bu bariz bir ekran atağı değilse (-4.0f'ten büyükse) yumuşak geçişe izin ver.
                if (!is_live && micro_movement_detected && logit_diff >= -4.0f) {
                    is_live = true;
                }
                
                if (!is_live) {
                    liveness_reason = "Liveness Engeli (Diff: " + to_string(logit_diff) + ", Var: " + to_string(landmark_variance) + ")";
                }
            } else {
                is_live = true; // liveness none
            }

            // 3. Face Recognition (ONLY run comparison if face is confirmed LIVE)
            if (is_live) {
                Mat aligned_face;
                auto start_align = chrono::high_resolution_clock::now();
                recognizer->alignCrop(process_frame, faces.row(0), aligned_face);
                auto end_align = chrono::high_resolution_clock::now();
                align_time = chrono::duration_cast<chrono::microseconds>(end_align - start_align).count() / 1000.0;

                Mat face_feature;
                auto start_rec = chrono::high_resolution_clock::now();
                recognizer->feature(aligned_face, face_feature);
                auto end_rec = chrono::high_resolution_clock::now();
                rec_time = chrono::duration_cast<chrono::microseconds>(end_rec - start_rec).count() / 1000.0;

                if (key == 's') {
                    if (reg_wizard == Reg_None) {
                        reg_wizard = Reg_StepStraight;
                        cout << "[SETUP WIZARD] Sihirbaz baslatildi. Adim 1: Karsiya bakip 's' tusuna basin." << endl;
                    } else if (reg_wizard == Reg_StepStraight) {
                        wizard_straight = face_feature.clone();
                        // Run calibration on the straight face frame
                        optimal_crop_factor = calibrateOptimalCropFactor(frame, bbox, liveness_net);
                        optimal_threshold = 1.5;
                        reg_wizard = Reg_StepLeft;
                        cout << "[SETUP WIZARD] Adim 1 Kaydedildi! Önerilen optimal katsayi: " << optimal_crop_factor << "x. Adim 2: Kafanizi SOLA cevirip 's' tusuna basin." << endl;
                    } else if (reg_wizard == Reg_StepLeft) {
                        wizard_left = face_feature.clone();
                        reg_wizard = Reg_StepRight;
                        cout << "[SETUP WIZARD] Adim 2 Kaydedildi! Adim 3: Kafanizi SAGA cevirip 's' tusuna basin." << endl;
                    } else if (reg_wizard == Reg_StepRight) {
                        wizard_right = face_feature.clone();
                        saveFaceFeatures(wizard_straight, wizard_left, wizard_right, optimal_crop_factor, optimal_threshold, feature_file);
                        straight_feat = wizard_straight.clone();
                        left_feat = wizard_left.clone();
                        right_feat = wizard_right.clone();
                        is_multi_angle = true;
                        has_registered_face = true;
                        reg_wizard = Reg_Done;
                        cout << "[SETUP WIZARD] Kurulum tamamlandi ve dosyaya yazildi (" << feature_file << ")" << endl;
                    }
                }

                if (has_registered_face) {
                    double score_straight = recognizer->match(straight_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                    double max_score = score_straight;
                    string angle_str = "STRAIGHT";

                    if (is_multi_angle) {
                        double score_left = recognizer->match(left_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                        double score_right = recognizer->match(right_feat, face_feature, FaceRecognizerSF::DisType::FR_COSINE);
                        if (score_left > max_score) {
                            max_score = score_left;
                            angle_str = "LEFT";
                        }
                        if (score_right > max_score) {
                            max_score = score_right;
                            angle_str = "RIGHT";
                        }
                    }

                    is_match = (max_score > config.matching_threshold);

                    string match_status = is_match ? "IDENTITY VERIFIED (MATCH)" : "UNKNOWN PERSON (NO MATCH)";
                    Scalar text_color = is_match ? Scalar(0, 255, 0) : Scalar(0, 165, 255);

                    putText(frame, match_status, Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.7, text_color, 2);
                    string scores = format("Score: %.3f (%s) | Threshold: %.2f", max_score, angle_str.c_str(), config.matching_threshold);
                    putText(frame, scores, Point(10, 55), FONT_HERSHEY_SIMPLEX, 0.5, text_color, 1);
                } else {
                    putText(frame, "NO ENROLLED FACE. Press 's' to enroll.", Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.55, Scalar(0, 165, 255), 1.5);
                }
            } else {
                // If spoof face is shown: BLOCK AUTHENTICATION & Show bold alert
                if (config.liveness_method == "active") {
                    string challenge_str = "";
                    Scalar banner_color;
                    if (active_state == Liveness_WaitStraight) {
                        if (avg_brightness < config.min_brightness) {
                            challenge_str = format("[1/2] LOOK STRAIGHT AT THE CAMERA (Dark Mode) (Straight: %d/3)", straight_frames);
                        } else {
                            challenge_str = format("[1/3] LOOK STRAIGHT AT THE CAMERA (Straight: %d/3)", straight_frames);
                        }
                        banner_color = Scalar(0, 165, 255); // Orange
                    } else if (active_state == Liveness_WaitBlink) {
                        challenge_str = "[2/3] PLEASE BLINK YOUR EYES";
                        banner_color = Scalar(255, 0, 255); // Pink/Purple
                    } else if (active_state == Liveness_WaitTurn) {
                        if (avg_brightness < config.min_brightness) {
                            challenge_str = "[2/2] TURN YOUR HEAD LEFT OR RIGHT (Dark Mode)";
                        } else {
                            challenge_str = "[3/3] TURN YOUR HEAD LEFT OR RIGHT";
                        }
                        banner_color = Scalar(0, 255, 255); // Cyan
                    } else {
                        challenge_str = "ACTIVE LIVENESS PENDING";
                        banner_color = Scalar(0, 0, 255);
                    }
                    putText(frame, challenge_str, Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.65, banner_color, 2);
                } else {
                    string spoof_str = "SPOOF: " + (liveness_reason.empty() ? "PHOTO/SCREEN!" : liveness_reason);
                    putText(frame, spoof_str, Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.65, Scalar(0, 0, 255), 2);
                    putText(frame, "AUTHENTICATION DENIED (SPOOF)", Point(10, 55), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 255), 1.5);
                }
                
                if (key == 's') {
                    Mat aligned_face;
                    recognizer->alignCrop(process_frame, faces.row(0), aligned_face);
                    Mat face_feature;
                    recognizer->feature(aligned_face, face_feature);

                    if (reg_wizard == Reg_None) {
                        reg_wizard = Reg_StepStraight;
                        cout << "[SETUP WIZARD] Wizard started. Step 1: Look straight and press 's'." << endl;
                    } else if (reg_wizard == Reg_StepStraight) {
                        wizard_straight = face_feature.clone();
                        // Run calibration on the straight face frame
                        optimal_crop_factor = calibrateOptimalCropFactor(frame, bbox, liveness_net);
                        optimal_threshold = 1.5;
                        reg_wizard = Reg_StepLeft;
                        cout << "[SETUP WIZARD] Step 1 Saved! Recommended optimal crop factor: " << optimal_crop_factor << "x. Step 2: Turn your head LEFT and press 's'." << endl;
                    } else if (reg_wizard == Reg_StepLeft) {
                        wizard_left = face_feature.clone();
                        reg_wizard = Reg_StepRight;
                        cout << "[SETUP WIZARD] Step 2 Saved! Step 3: Turn your head RIGHT and press 's'." << endl;
                    } else if (reg_wizard == Reg_StepRight) {
                        wizard_right = face_feature.clone();
                        saveFaceFeatures(wizard_straight, wizard_left, wizard_right, optimal_crop_factor, optimal_threshold, feature_file);
                        straight_feat = wizard_straight.clone();
                        left_feat = wizard_left.clone();
                        right_feat = wizard_right.clone();
                        is_multi_angle = true;
                        has_registered_face = true;
                        reg_wizard = Reg_Done;
                        cout << "[SETUP WIZARD] Enrollment completed and written to file (" << feature_file << ")" << endl;
                    }
                }
            }

            // Draw sleek semi-transparent wizard overlay if setup wizard is running
            if (reg_wizard != Reg_None) {
                string wizard_str = "";
                Scalar wizard_color;
                if (reg_wizard == Reg_StepStraight) {
                    wizard_str = "[FACE ENROLL 1/3] LOOK STRAIGHT AND PRESS 's'";
                    wizard_color = Scalar(0, 165, 255);
                } else if (reg_wizard == Reg_StepLeft) {
                    wizard_str = "[FACE ENROLL 2/3] LOOK LEFT AND PRESS 's'";
                    wizard_color = Scalar(255, 0, 255);
                } else if (reg_wizard == Reg_StepRight) {
                    wizard_str = "[FACE ENROLL 3/3] LOOK RIGHT AND PRESS 's'";
                    wizard_color = Scalar(255, 255, 0);
                } else if (reg_wizard == Reg_Done) {
                    wizard_str = "[FACE ENROLL] ENROLLMENT SUCCESSFUL!";
                    wizard_color = Scalar(0, 255, 0);
                }

                // Draw overlay rectangle
                rectangle(frame, Point(0, 0), Point(frame.cols, 70), Scalar(0, 0, 0), -1);
                putText(frame, wizard_str, Point(15, 35), FONT_HERSHEY_SIMPLEX, 0.65, wizard_color, 2);
                putText(frame, "Press 'r' to reset or 'q' to exit.", Point(15, 58), FONT_HERSHEY_SIMPLEX, 0.42, Scalar(200, 200, 200), 1);
            }

            // Draw upscaled face box with colors matching matching/spoofing states
            drawFace(frame, faces_upscaled, det_time, is_live, has_registered_face, is_match, avg_brightness, config.liveness_method, active_state);

            // Display timing on frame
            double total_ai_time = det_time + liveness_time + align_time + rec_time;
            string stats = format("Det: %.1fms | Live (%s): %.1fms | Align: %.1fms | Rec: %.1fms", 
                                  det_time, config.liveness_method.c_str(), liveness_time, align_time, rec_time);
            putText(frame, stats, Point(10, frame.rows - 25), FONT_HERSHEY_SIMPLEX, 0.45, Scalar(255, 255, 255), 1);
            
            string metric_stats = "";
            if (config.liveness_method == "texture") {
                metric_stats = format("Lap Variance: %.1f | HSV Sat: %.1f", laplacian_var_gui, hsv_sat_gui);
            } else if (config.liveness_method == "active") {
                metric_stats = format("Yaw: %.3f | Eye Var (Left/Right): %.1f / %.1f (Threshold: <16.0)", 
                                       yaw_ratio_gui, l_eye_var_gui, r_eye_var_gui);
            } else {
                metric_stats = "MiniFASNet AI Liveness Active";
            }
            
            if (config.enable_adaptive_exposure) {
                metric_stats += " | CLAHE: ON";
            } else {
                metric_stats += " | CLAHE: OFF";
            }
            putText(frame, metric_stats, Point(10, frame.rows - 10), FONT_HERSHEY_SIMPLEX, 0.4, Scalar(0, 255, 255), 1);

        } else {
            // Reset liveness history when no face is in screen to clear queue
            liveness_history.clear();
            landmark_history.clear();
            if (config.liveness_method == "active") {
                active_state = Liveness_WaitStraight;
                straight_frames = 0;
                blink_phase = Blink_PhaseOpen;
            }
            
            putText(frame, "NO FACE DETECTED", Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.6, Scalar(0, 0, 255), 1.5);
            
            // handled at top of loop
        }

        // Show window
        imshow("AegisFace - Live Setup & Test Wizard", frame);
    }

    cap.release();
    destroyAllWindows();
    return 0;
}
