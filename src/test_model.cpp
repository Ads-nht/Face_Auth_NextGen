#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>

using namespace std;
using namespace cv;

int main() {
    string model_path = "models/minifas.onnx";
    dnn::Net net = dnn::readNetFromONNX(model_path);
    if (net.empty()) {
        cerr << "Model could not be loaded!" << endl;
        return -1;
    }
    
    Mat dummy = Mat::ones(128, 128, CV_8UC3) * 255;
    
    Mat float_img;
    dummy.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
    
    // Normalize each channel (standard PyTorch ImageNet stats)
    vector<Mat> channels(3);
    split(float_img, channels);
    channels[0] = (channels[0] - 0.485) / 0.229; // R
    channels[1] = (channels[1] - 0.456) / 0.224; // G
    channels[2] = (channels[2] - 0.406) / 0.225; // B
    merge(channels, float_img);
    
    Mat inputBlob = dnn::blobFromImage(float_img, 1.0, Size(128, 128), Scalar(0, 0, 0), false, false);
    
    net.setInput(inputBlob);
    Mat output = net.forward();
    
    cout << "Output rows: " << output.rows << ", cols: " << output.cols << endl;
    cout << "Output dimensions: " << output.size << endl;
    
    for (int i = 0; i < output.cols; ++i) {
        cout << "Index [" << i << "]: " << output.at<float>(0, i) << endl;
    }
    
    return 0;
}
