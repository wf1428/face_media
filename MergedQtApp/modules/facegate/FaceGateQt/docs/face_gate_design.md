# FaceGateQt migration design

Source reference: `face_test.cpp` is the preserved copy of `face_test_threaded.cpp`.

## First module split

- `CameraService`: owns V4L2 open, mplane mmap, STREAMON/STREAMOFF and NV12/NV21 to RGB888 preview frames.
- `FaceEngine`: owns InspireFace launch/session/feature matching. SDK handles must stay behind this class.
- `LivenessWorker`: owns MiniFASNetV2 and MiniFASNetV1SE RKNN contexts. The worker keeps only the newest request.
- `VerificationController`: owns the gate verification state machine.
- `MysqlFaceRepository`: stores persons, features and verification logs in MySQL.
- `MainWindow`: only displays UI and wires signals.

## Verification policy

- Collect 5 matched face frames.
- Select the best frame by `quality * 0.6 + detConfidence * 0.3 + faceSizeScore * 0.1`.
- Run 5 liveness checks.
- Hold pass/fail result for 2000 ms before returning to idle.

## Build switches

`FaceGateQt.pro` keeps SDK bindings optional:

```bash
INSPIREFACE_ROOT=/opt/inspireface RKNN_ROOT=/usr qmake FaceGateQt.pro
make
```

Without those variables, the UI, database shell and thread wiring still build, while the actual SDK/RKNN inference reports disabled status.
