# 🛡️ AegisFace (FaceAuth NextGen)
### Next-Gen, Ultra-Fast (<10ms) Face Authentication PAM Module for Linux

AegisFace is a high-performance, secure, and lightweight face authentication system designed as a C++ / Pure C replacement and spiritual successor to [Howdy](https://github.com/boltgolt/howdy). Built with a **Daemon-Client architecture** and optimized ONNX models via OpenCV DNN, it provides near-instantaneous authentication with advanced, multi-layered anti-spoofing technology.

---

## 🚀 Key Features

* **⚡ Ultra-Low Latency:** Average face recognition loop runs in **9.6 ms** (daemon-side) using cached models and multi-threaded OpenCV DNN inferences (`cv::setNumThreads`).
* **🔄 Daemon-Client Architecture:** Neural network models are kept resident in memory inside a system background service (`faceauth_daemon`), communicating with the client via a local UNIX socket (`/run/faceauth/faceauth.sock`). This avoids model-loading overhead and bypasses sandboxing limitations (e.g., SDDM Greeter home directory isolation).
* **🔒 Pure C PAM Module:** The PAM client (`pam_faceauth.so`) is written in pure C. This removes the `libstdc++` runtime dependency during authentication, completely preventing the notorious `sudo` segmentation faults (`SIGSEGV`) during `dlclose()` calls.
* **🛡️ Advanced Anti-Spoofing (Liveness & Attack Mitigation):**
  * **Yapay Zeka (MiniFASNet ONNX):** Passive liveness detection that scans and blocks spoofing attempts from printed paper photos (still images) and video playbacks from smartphone/tablet screens.
  * **Texture & Edge Analysis:** High-speed backup liveness method utilizing Laplacian Variance (to detect paper blur on printed photos and high-frequency Moire grids on digital screens) and HSV skin color saturation ranges.
  * **Rigid Eye-Nose Alignment:** Landmarks are relative to the nose-tip, neutralizing 2D perspective shifts.
  * **Physiological Jitter Boundaries:** Imposes lower (`1.5e-5`) and upper (`8.0e-4`) variance thresholds to allow normal human micro-movement while blocking static photos and high-frequency shake bypasses.
  * **Socket Privilege Verification (Poka-Yoke):** Verifies the client process's UID using Linux `SO_PEERCRED` on the UNIX socket. Rejects any authentication requests where the socket client is not root (UID 0), not a system display manager, and does not match the target user's UID. This prevents local privilege escalation via socket spoofing.
* **⚙️ Adaptive Exposure & Calibration:** Enrolls user faces with an auto-calibration loop scanning local lighting conditions for optimal crop factors, with optional CLAHE contrast equalization for low-light/night environments.

---

## 🛠️ Architecture

```mermaid
graph TD
    User([User / Process]) -->|Triggers Auth| PAM[pam_faceauth.so (Pure C Client)]
    PAM -->|Connects via Unix Socket| Sock[Unix Socket: /run/faceauth/faceauth.sock]
    Sock -->|Auth Request| Daemon[faceauth_daemon (C++ Service)]
    Daemon -->|Captures Frame| Cam[V4L2 Camera Device]
    Daemon -->|Runs Inference| Models[ONNX Models: YuNet, SFace, MiniFASNet]
    Models -->|Verifies Liveness & Features| Daemon
    Daemon -->|Returns SUCCESS / FAIL| PAM
    PAM -->|Grants/Denies Access| User
```

---

## 📋 System Requirements

* **Operating System:** Linux (Hyprland, KDE, GNOME, etc. compatible)
* **Compiler:** GCC/G++ with C++17 support, CMake (>= 3.10)
* **Libraries:** 
  * OpenCV 4.x (compiled with DNN, VideoIO, Imgproc modules)
  * Linux PAM development headers (`libpam0g-dev` on Debian/Ubuntu, `pam` on Arch/CachyOS)

---

## ⚙️ Build & Installation

### 1. Compile the Project
To compile the daemon, benchmark tools, and the PAM shared library:
```bash
cmake -B build
cmake --build build
```

### 2. Run the Secure Installer
The installer (`setup.sh`) configures the system directories, sets permissions, copies the models, and configures the systemd service:
```bash
sudo ./setup.sh
```

---

## 👤 Enrollment & User Setup

To register your face signature, run the interactive GUI utility:
```bash
sudo faceauth_live_test <username>
```
* Look directly at the camera.
* The system will sweep exposure rates and check liveness.
* Press **`s`** to save your profile (stored securely under `/var/lib/faceauth/users/<username>.yml`).
* Press **`q`** to quit.

---

## 🔐 PAM Integration

To enable face auth for `sudo` or locking screens, edit your PAM configuration files.

### For `sudo`:
Open `/etc/pam.d/sudo`:
```bash
sudo nano /etc/pam.d/sudo
```
Add the following line right at the top (as the first auth rule):
```ini
auth        sufficient  pam_faceauth.so
```

### For Hyprlock / SDDM / GDM:
Similarly, add the line in `/etc/pam.d/hyprlock` or `/etc/pam.d/sddm`:
```ini
auth        sufficient  pam_faceauth.so
```
*Note: Using `sufficient` ensures that if face auth succeeds, you are logged in. If it fails (or the daemon is stopped), it falls back gracefully to your standard password input.*

---

## 🛠️ Configuration `/etc/faceauth.conf`

The behavior of the daemon can be tuned dynamically. Parameters are re-read on every authentication request (no daemon restart required):

```ini
[General]
# Anti-spoofing method: "minifas" (ONNX AI Model), "texture" (CV Laplacian/HSV), or "none"
liveness_method = minifas
# Cosine similarity threshold for face verification (0.0 to 1.0)
matching_threshold = 0.45

[Texture]
# Edge variance limits for photo/screen detection
texture_min_laplacian = 380.0
texture_max_laplacian = 1500.0
texture_min_hsv_sat = 95.0
texture_max_hsv_sat = 190.0

[MiniFASNet]
# logit difference threshold (Real logit - Spoof logit)
minifas_threshold = 1.5

[Environment]
# Minimum average brightness required for auth (0 - 255)
min_brightness = 35.0
# Adaptive lighting equalization using CLAHE
enable_adaptive_exposure = true
```

---

## 📄 License

This project is licensed under the MIT License.

---

## 💖 Acknowledgments & Inspiration

AegisFace is heavily inspired by [Howdy](https://github.com/boltgolt/howdy), the popular Python-based face authentication system for Linux. 

While AegisFace is a complete rewrite from scratch in C++ and C to resolve Howdy's startup latency, python dependency overhead, and dynamic linker crashes (`sudo` segfaults), it honors Howdy's original vision of bringing seamless, secure face unlock to the Linux ecosystem. Thank you to the original creators and contributors of Howdy!
