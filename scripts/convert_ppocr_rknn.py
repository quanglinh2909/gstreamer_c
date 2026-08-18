#!/usr/bin/env python3
"""Chuyen model PaddleOCR (ONNX) sang .rknn dung chuan loai model `paddle_ocr`.

Chay THANG TREN BOARD: rknn-toolkit2 2.3.0 da cai san o ~/.local.

    python3 convert_ppocr_rknn.py \
        --det  plate_det.onnx \
        --rec  plate_rec.onnx \
        --dict vn_plate_dict.txt \
        --name plate

-> weights/plate_det.rknn, weights/plate_rec.rknn, weights/plate_rec.txt

VI SAO PHAI DUNG SCRIPT NAY chu khong dung convert.py cua rknn_model_zoo:
engine bom thang pixel 0..255 vao model, nen phep chuan hoa PHAI nam trong
model. Zoo de phan chuan hoa cho code Python goi, convert theo no thi dau vao
lech 255 lan va model doc ra rac.

  det: mean/std ImageNet                (dung cho ca PPOCR-Det cua zoo lan
                                         model tu train — xem config.yml)
  rec: xem REC_NORM ben duoi — hai kieu, mac dinh xuat CA HAI de thu

Doi chieu hai ban rec: mo /model-test, tai mot anh DA CAT SAT MOT DONG chu,
chon loai `paddle_ocr_rec` roi chay lan luot `<ten>_rec.rknn` va
`<ten>_pm1_rec.rknn` — ban nao ra chu that thi giu, ban kia xoa di.

Ten file phai theo cap `<ten>_det.rknn` / `<ten>_rec.rknn`: loai model gop
`paddle_ocr` chi nhan duong dan DET roi tu suy ra REC va tu dien `<rec>.txt`.

--------------------------------------------------------------------------
CHI CO CHECKPOINT PADDLE (.pdparams) THI LAM SAO? Xuat ONNX ngay tren board:

  pip install virtualenv  # da co san
  virtualenv -p python3.10 ~/Documents/paddle_env
  ~/Documents/paddle_env/bin/pip install paddlepaddle==2.6.2 \
      pyyaml "numpy<2" opencv-python-headless shapely pyclipper Pillow \
      six requests scikit-image imgaug lmdb "paddle2onnx==1.3.1"
  git clone --depth 1 -b release/2.9 \
      https://github.com/PaddlePaddle/PaddleOCR ~/Documents/PaddleOCR

  # PaddleOCR 2.9 keo theo albumentations chi de doc cong thuc toan; boc
  # `from .latex_ocr_aug import *` trong ppocr/data/imaug/__init__.py vao
  # try/except roi bo qua.

  # sua config.yml cua chinh lan train do: use_gpu false, pretrained_model tro
  # vao best_accuracy (khong co duoi), save_inference_dir, character_dict_path
  cd ~/Documents/PaddleOCR
  ~/Documents/paddle_env/bin/python tools/export_model.py -c det.yml
  ~/Documents/paddle_env/bin/python tools/export_model.py -c rec.yml
  ~/Documents/paddle_env/bin/paddle2onnx --model_dir inference/plate_det \
      --model_filename inference.pdmodel --params_filename inference.pdiparams \
      --save_file plate_det.onnx --opset_version 12

paddle2onnx BAN 2.x doi paddle >= 3.0 va format PIR moi, khong doc duoc
inference.pdmodel cua paddle 2.6 — phai dung dung 1.3.1.

MAT FILE TU DIEN thi doc nguoc tu chinh model: so lop dau ra cua rec = 1 +
so ky tu, va tu dien PaddleOCR luon xep theo thu tu, nen ve tung ky tu roi xem
model xep vao lop nao la suy ra du bo. Bien so VN: 10 chu so + 20 chu cai
`ABCDEFGHKLMNPSTUVXYZ` (bo I J O Q R W) = dung 30 ky tu.
"""
import argparse
import os
import shutil
import sys

# Hai kieu chuan hoa cua nhanh rec, PHAI chon dung kieu ma model duoc huan
# luyen, sai la doc ra rac:
#   "01"  -> anh ve [0,1]      (ban ONNX Rockchip/PaddlePaddle phat hanh — DA DO)
#   "pm1" -> anh ve [-1,1]     (resize_norm_img cua PaddleOCR: /255, -0.5, /0.5
#                               — kieu cua model tu huan luyen roi export)
# Khong doan duoc tu file ONNX; convert ca hai roi thu tren anh that la nhanh
# nhat (mot ban ra chu, ban kia ra ky tu ngau nhien).
REC_NORM = {
    "01": ([[0, 0, 0]], [[255, 255, 255]]),
    "pm1": ([[127.5, 127.5, 127.5]], [[127.5, 127.5, 127.5]]),
}
DET_MEAN = [[123.675, 116.28, 103.53]]
DET_STD = [[58.395, 57.12, 57.375]]


def convert(onnx_path, out_path, shape, mean, std, name, dataset=None,
            quant_algorithm="normal"):
    """dataset != None -> lam INT8, dung dung bo anh do engine dump ra.

    VI SAO INT8 DANG GIA: model fp16 khong chi cham o rknn_run — `rknn_inputs_set`
    con phai doi uint8 -> fp16 bang CPU cho tung phan tu. Do tren plate_det
    480x480: inputs_set 19,6 ms + run 37,2 ms. Ban int8 cat ca hai.

    BO ANH HIEU CHUAN PHAI LA DAU VAO THAT cua tang do, khong phai anh bien so
    tho: dau vao cua det da qua letterbox + dem xam, con dau vao cua rec la mot
    DONG chu do det cat ra roi fit-height. Lay bang cach dat AI_PPOCR_DUMP_DIR
    cho engine (xem src/ai/detect_ai/ppocr_dump.h) roi doi .bin sang PNG.

    LUU Y THU TU KENH: rknn-toolkit2 doc anh bang cv2.imread (BGR) roi cvtColor
    sang RGB, tuc file PNG phai chua RGB THAT. Engine dump ra RGB nen luc ghi
    PNG bang cv2.imwrite phai dao kenh (`arr[:, :, ::-1]`)."""
    if not os.path.isfile(onnx_path):
        print(f"  [{name}] khong thay file {onnx_path}")
        return False
    if dataset is not None and not os.path.isfile(dataset):
        print(f"  [{name}] khong thay dataset {dataset}")
        return False

    from rknn.api import RKNN

    def new_rknn():
        r = RKNN(verbose=False)
        r.config(mean_values=mean, std_values=std, target_platform="rk3588",
                 quantized_algorithm=quant_algorithm)
        return r

    rknn = new_rknn()
    # inputs/input_size_list ep shape co dinh: ONNX cua PaddleOCR khai bao chieu
    # dong (dynamic), ma NPU chi chay shape tinh.
    if rknn.load_onnx(model=onnx_path, inputs=["x"], input_size_list=[shape]) != 0:
        # Mot so ban export dat ten dau vao khac 'x' -> thu khong ep ten.
        rknn = new_rknn()
        if rknn.load_onnx(model=onnx_path) != 0:
            print(f"  [{name}] KHONG nap duoc ONNX")
            return False
    if rknn.build(do_quantization=dataset is not None, dataset=dataset) != 0:
        print(f"  [{name}] build that bai")
        return False
    if rknn.export_rknn(out_path) != 0:
        print(f"  [{name}] export that bai")
        return False
    rknn.release()
    print(f"  [{name}] -> {out_path} ({os.path.getsize(out_path)/1e6:.1f} MB)")
    return True


def smoke_test(path, hw):
    """Nap lai bang runtime that va do thoi gian — convert xong khong co nghia
    la chay duoc: phien ban runtime, layout dau ra, op roi ve CPU deu chi lo ra
    o day."""
    import time

    import numpy as np
    from rknnlite.api import RKNNLite

    r = RKNNLite(verbose=False)
    if r.load_rknn(path) != 0 or r.init_runtime() != 0:
        print(f"  [{os.path.basename(path)}] KHONG chay duoc tren NPU")
        return
    x = (np.random.rand(1, hw[0], hw[1], 3) * 255).astype(np.uint8)
    out = r.inference(inputs=[x], data_format="nhwc")
    for _ in range(3):
        r.inference(inputs=[x], data_format="nhwc")
    t0 = time.perf_counter()
    for _ in range(20):
        r.inference(inputs=[x], data_format="nhwc")
    ms = (time.perf_counter() - t0) / 20 * 1000
    print(f"  [{os.path.basename(path)}] {ms:.1f} ms/lan, dau ra {[o.shape for o in out]}")
    r.release()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--det", help="ONNX cua model det")
    ap.add_argument("--rec", help="ONNX cua model rec")
    ap.add_argument("--dict", dest="dict_path", help="file tu dien ky tu cua rec")
    ap.add_argument("--name", default="plate", help="ten chung, vd 'plate'")
    ap.add_argument("--out", default="weights", help="thu muc dich")
    ap.add_argument("--det-size", default="640x640", help="WxH dau vao det")
    ap.add_argument("--rec-size", default="320x48", help="WxH dau vao rec")
    ap.add_argument("--rec-norm", default="both", choices=["01", "pm1", "both"],
                    help="kieu chuan hoa cua rec; 'both' xuat ca hai de thu")
    ap.add_argument("--det-dataset", help="dataset.txt de LUONG TU HOA int8 nhanh det")
    ap.add_argument("--rec-dataset", help="dataset.txt de LUONG TU HOA int8 nhanh rec")
    ap.add_argument("--quant-algorithm", default="normal",
                    choices=["normal", "mmse", "kl_divergence"],
                    help="thuat toan chon dai luong tu; mmse cham hon nhieu"
                         " nhung thuong giu do chinh xac tot hon")
    args = ap.parse_args()

    if not args.det and not args.rec:
        ap.error("can it nhat --det hoac --rec")
    os.makedirs(args.out, exist_ok=True)

    dw, dh = (int(v) for v in args.det_size.lower().split("x"))
    rw, rh = (int(v) for v in args.rec_size.lower().split("x"))

    ok = True
    if args.det:
        p = os.path.join(args.out, f"{args.name}_det.rknn")
        ok &= convert(args.det, p, [1, 3, dh, dw], DET_MEAN, DET_STD, "det",
                      dataset=args.det_dataset, quant_algorithm=args.quant_algorithm)
        if ok:
            smoke_test(p, (dh, dw))
    if args.rec:
        kinds = ["01", "pm1"] if args.rec_norm == "both" else [args.rec_norm]
        for i, kind in enumerate(kinds):
            mean, std = REC_NORM[kind]
            # Ban dau tien giu ten chuan `<ten>_rec.rknn` (loai model gop tu suy
            # ra tu file det); ban con lai them hau to de thu doi chieu.
            suffix = "" if i == 0 else f"_{kind}"
            p = os.path.join(args.out, f"{args.name}{suffix}_rec.rknn")
            if convert(args.rec, p, [1, 3, rh, rw], mean, std, f"rec[{kind}]",
                       dataset=args.rec_dataset,
                       quant_algorithm=args.quant_algorithm):
                smoke_test(p, (rh, rw))
            else:
                ok = False
    if args.dict_path:
        dst = os.path.join(args.out, f"{args.name}_rec.txt")
        shutil.copyfile(args.dict_path, dst)
        if args.rec and args.rec_norm == "both":
            shutil.copyfile(args.dict_path, os.path.join(args.out, f"{args.name}_pm1_rec.txt"))
        with open(dst, encoding="utf-8") as f:
            n = sum(1 for _ in f)
        print(f"  [dict] -> {dst} ({n} ky tu)")
        print("  LUU Y: so lop dau ra cua rec phai = 1 (blank) + so ky tu"
              " tu dien (+1 neu train voi use_space_char)")

    print("\nXong." if ok else "\nCO LOI, xem tren.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
