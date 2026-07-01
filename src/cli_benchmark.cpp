#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>

using namespace std;
using namespace cv;

int main() {
    string det_model_path = "models/yunet.onnx";
    string rec_model_path = "models/sface.onnx";

    cout << "\n========================================================" << endl;
    cout << "   AegisFace - Terminal Benchmark (CLI) v1.0          " << endl;
    cout << "========================================================" << endl;
    cout << "[INFO] Loading models..." << endl;

    Ptr<FaceDetectorYN> detector = FaceDetectorYN::create(det_model_path, "", Size(320, 240), 0.6f, 0.3f, 5000);
    Ptr<FaceRecognizerSF> recognizer = FaceRecognizerSF::create(rec_model_path, "");

    if (detector.empty() || recognizer.empty()) {
        cerr << "[ERROR] Failed to load models! Are the file paths correct?" << endl;
        return -1;
    }
    cout << "[OK] Models successfully loaded." << endl;

    cout << "[INFO] Opening webcam (ID: 0)..." << endl;
    VideoCapture cap(0);
    if (!cap.isOpened()) {
        cerr << "[ERROR] Failed to open webcam! Please check the connection." << endl;
        return -1;
    }
    cout << "[OK] Camera opened. Starting benchmark over 30 frames...\n" << endl;

    // Warm-up
    Mat dummy = Mat::zeros(240, 320, CV_8UC3);
    Mat dummy_faces;
    detector->detect(dummy, dummy_faces);
    cout << "[INFO] Model warm-up completed. Starting benchmark test...\n" << endl;

    vector<double> det_times;
    vector<double> align_times;
    vector<double> rec_times;
    vector<double> total_times;
    int faces_detected_count = 0;

    for (int frame_idx = 1; frame_idx <= 30; ++frame_idx) {
        Mat frame;
        cap >> frame;
        if (frame.empty()) {
            cerr << "[WARNING] Received empty frame! Skipping..." << endl;
            continue;
        }

        Mat process_frame;
        resize(frame, process_frame, Size(320, 240));
        detector->setInputSize(process_frame.size());

        Mat faces;
        auto t0 = chrono::high_resolution_clock::now();
        
        // 1. Detection
        detector->detect(process_frame, faces);
        auto t1 = chrono::high_resolution_clock::now();
        double det_t = chrono::duration_cast<chrono::microseconds>(t1 - t0).count() / 1000.0;
        
        double align_t = 0;
        double rec_t = 0;

        if (faces.rows > 0) {
            faces_detected_count++;
            
            // 2. Alignment
            auto t2 = chrono::high_resolution_clock::now();
            Mat aligned_face;
            recognizer->alignCrop(process_frame, faces.row(0), aligned_face);
            auto t3 = chrono::high_resolution_clock::now();
            align_t = chrono::duration_cast<chrono::microseconds>(t3 - t2).count() / 1000.0;

            // 3. Feature Extraction
            auto t4 = chrono::high_resolution_clock::now();
            Mat face_feature;
            recognizer->feature(aligned_face, face_feature);
            auto t5 = chrono::high_resolution_clock::now();
            rec_t = chrono::duration_cast<chrono::microseconds>(t5 - t4).count() / 1000.0;
        }

        double total_t = det_t + align_t + rec_t;

        det_times.push_back(det_t);
        if (faces.rows > 0) {
            align_times.push_back(align_t);
            rec_times.push_back(rec_t);
        }
        total_times.push_back(total_t);

        cout << format("Frame %2d/30 | Face Detect: %s | Det: %5.1f ms | Align: %5.1f ms | Rec: %5.1f ms | Total: %5.1f ms",
                       frame_idx, (faces.rows > 0 ? "YES" : "NO"), det_t, align_t, rec_t, total_t) << endl;
    }

    cap.release();

    // Calculate Stats
    auto calc_stats = [](const vector<double>& v, double& min_val, double& max_val, double& avg_val) {
        if (v.empty()) {
            min_val = max_val = avg_val = 0;
            return;
        }
        min_val = *min_element(v.begin(), v.end());
        max_val = *max_element(v.begin(), v.end());
        avg_val = accumulate(v.begin(), v.end(), 0.0) / v.size();
    };

    double min_det, max_det, avg_det;
    double min_align, max_align, avg_align;
    double min_rec, max_rec, avg_rec;
    double min_total, max_total, avg_total;

    calc_stats(det_times, min_det, max_det, avg_det);
    calc_stats(align_times, min_align, max_align, avg_align);
    calc_stats(rec_times, min_rec, max_rec, avg_rec);
    calc_stats(total_times, min_total, max_total, avg_total);

    cout << "\n========================================================" << endl;
    cout << "             BENCHMARK RESULTS (STATS)                  " << endl;
    cout << "========================================================" << endl;
    cout << format("Total Frames Processed: 30") << endl;
    cout << format("Faces Detected        : %d frames", faces_detected_count) << endl;
    cout << "--------------------------------------------------------" << endl;
    cout << " Process Step       |  Minimum  |  Maximum  |  Average  " << endl;
    cout << "--------------------|-----------|-----------|-----------" << endl;
    cout << format(" Face Detect (Det)  | %5.1f ms  | %5.1f ms  | %5.1f ms", min_det, max_det, avg_det) << endl;
    cout << format(" Alignment (Align)  | %5.1f ms  | %5.1f ms  | %5.1f ms", min_align, max_align, avg_align) << endl;
    cout << format(" Recognition (Rec)  | %5.1f ms  | %5.1f ms  | %5.1f ms", min_rec, max_rec, avg_rec) << endl;
    cout << "--------------------|-----------|-----------|-----------" << endl;
    cout << format(" TOTAL DURATION     | %5.1f ms  | %5.1f ms  | %5.1f ms", min_total, max_total, avg_total) << endl;
    cout << "========================================================" << endl;
    cout << "[INFO] Prototype C++ benchmark test completed." << endl;

    return 0;
}
