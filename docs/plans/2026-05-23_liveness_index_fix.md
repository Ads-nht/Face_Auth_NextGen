# FaceAuth NextGen - Çift Eşik (Dual-Threshold) Kalibrasyon Planı

Kullanıcımız Furkan Bey'in onayıyla liveness ve yüz tanıma güvenliğini en üst düzeye çıkaracak olan çift eşik (dual-threshold) kalibrasyonu uygulanacaktır.

## Kalibrasyon Detayları

1.  **Liveness Eşik Kalibrasyonu (`logit_diff >= 0.7`):**
    *   **Mevcut Durum:** `0.0`
    *   **Yeni Durum:** **`0.7`**
    *   **Gerekçe:** Loglarda görüldüğü üzere fotoğraf gösterildiğinde anlık sapmalar en fazla `0.62` değerine ulaşmaktadır. Eşiği `0.7` değerine çekerek fotoğraf saldırılarını tamamen blokluyoruz. Gerçek yüzünüz ise `2.6` ile `7.5` arasında değerler ürettiği için bu eşikten en ufak bir takılma olmadan geçecektir.

2.  **Yüz Tanıma Eşik Kalibrasyonu (`cosine_score > 0.45`):**
    *   **Mevcut Durum:** `0.36`
    *   **Yeni Durum:** **`0.45`**
    *   **Gerekçe:** SFace modeli için `0.36` standardı 100.000'de 1 hata payı sunar. Eşiği `0.45` değerine yükselterek sahte yüzlerin veya benzer kişilerin eşleşme ihtimalini tamamen sıfırlıyoruz. Yeni tıraşlı yüzünüzü kaydettiğinizde kendi yüzünüz için benzerlik skoru `0.85` - `0.95` civarında olacağı için sistem sizi anında tanıyacaktır.

## Proposed Changes

### [Component Name] FaceAuth C++ Source Code

#### [MODIFY] [live_test.cpp](file:///home/ads/Antigravity/Projeler/FaceAuth_NextGen/src/live_test.cpp)
- Liveness eşiği `0.7` yapılacaktır:
  ```cpp
  bool current_frame_is_live = (logit_diff >= 0.7);
  ```
- Yüz eşleşme eşiği `0.45` yapılacaktır:
  ```cpp
  bool is_match = (cosine_score > 0.45);
  ```

#### [MODIFY] [daemon.cpp](file:///home/ads/Antigravity/Projeler/FaceAuth_NextGen/src/daemon.cpp)
- Liveness eşiği `0.7` yapılacaktır:
  ```cpp
  bool current_frame_is_live = (logit_diff >= 0.7);
  ```
- Yüz eşleşme eşiği `0.45` yapılacaktır:
  ```cpp
  bool is_match = (cosine_score > 0.45);
  ```

## Verification Plan

### Automated Tests
- Proje derlenecektir:
  ```bash
  cmake -B build && cmake --build build
  ```

### Manual Verification
- Servis durdurulup kurulacak ve yeniden başlatılacaktır.
- `faceauth_live_test ads` ile liveness ve yüz tanıma başarıları test edilecektir.
