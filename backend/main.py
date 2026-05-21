import os

import uuid

import fitz # PyMuPDF

import subprocess # NEW import to run command-line tools

from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi import Form
from replicate import Client

from fastapi.middleware.cors import CORSMiddleware

from fastapi.staticfiles import StaticFiles

from pydantic import BaseModel

from typing import List
import shutil # <-- add this for file operations
from PIL import Image
import numpy as np
import trimesh
import uuid as _uuid

import requests, zipfile, io, tempfile
from pathlib import Path
import replicate

import traceback
from dotenv import load_dotenv
import json  # NEW

# Load backend/.env explicitly
load_dotenv(dotenv_path=Path(__file__).parent / ".env")

# --- SETUP ---

TEMP_UPLOADS_DIR = "temp_uploads"

PREVIEWS_DIR = "static/previews"

THREED_MODELS_DIR = "static/3dmodels" # <-- new directory for 3d models

RECORDINGS_DIR = "static/recordings"  # NEW: output videos/cleaned audio
RECORDINGS_INDEX = Path(RECORDINGS_DIR) / "recordings_index.json"  # NEW: simple metadata store

os.makedirs(TEMP_UPLOADS_DIR, exist_ok=True)

os.makedirs(PREVIEWS_DIR, exist_ok=True)

os.makedirs(THREED_MODELS_DIR, exist_ok=True) # <-- ensure directory exists

os.makedirs(RECORDINGS_DIR, exist_ok=True)  # NEW


# --- IMPORTANT: Path to LibreOffice ---

# On Windows, the command might not be in the system PATH.

# Update this path if your LibreOffice is installed elsewhere.

SOFFICE_PATH = "C:\\Program Files\\LibreOffice\\program\\soffice.exe"

# ------------------------------------

app = FastAPI()

app.add_middleware(

    CORSMiddleware,

    allow_origins=["http://localhost:8080", "http://localhost:5173", "http://localhost:3000"],

    allow_credentials=True,

    allow_methods=["*"],

    allow_headers=["*"],

)

app.mount("/static", StaticFiles(directory="static"), name="static")


# --- HELPER FUNCTIONS ---


def convert_office_to_pdf(file_path: str, output_dir: str):

    """Converts an Office document to PDF using LibreOffice."""

    try:

        # Check if soffice path exists

        if not os.path.exists(SOFFICE_PATH):

            print(f"ERROR: LibreOffice not found at {SOFFICE_PATH}")

            return None

            

        command = [

            SOFFICE_PATH,

            '--headless',

            '--convert-to', 'pdf',

            '--outdir', output_dir,

            file_path

        ]

        subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        

        pdf_filename = os.path.splitext(os.path.basename(file_path))[0] + ".pdf"

        return os.path.join(output_dir, pdf_filename)

    except subprocess.CalledProcessError as e:

        print(f"Error during Office to PDF conversion: {e.stderr.decode()}")

        return None

    except FileNotFoundError:

        print(f"ERROR: Could not find LibreOffice. Please check the SOFFICE_PATH variable.")

        return None



def process_pdf_to_images(file_path: str, original_filename: str):

    slides = []

    try:

        doc = fitz.open(file_path)

        for page_num in range(len(doc)):

            page = doc.load_page(page_num)

            pix = page.get_pixmap()

            preview_filename = f"{os.path.splitext(original_filename)[0]}_{uuid.uuid4()}.png"

            preview_filepath = os.path.join(PREVIEWS_DIR, preview_filename)

            pix.save(preview_filepath)

            

            slides.append({

                "source": original_filename,

                "page": page_num + 1,

                "previewUrl": f"/static/previews/{preview_filename}"

            })

        doc.close()

    except Exception as e:

        print(f"Error processing PDF {original_filename}: {e}")

    return slides


def make_heightmap_glb(image_path: str, out_dir: str) -> str:
    """
    Simple local 2D->3D: brightness => height. Exports a valid .glb.
    """
    img = Image.open(image_path).convert("RGB")
    # Keep meshes small for the browser
    max_res = 160
    w, h = img.size
    scale = min(max_res / max(w, h), 1.0)
    nw, nh = max(8, int(w * scale)), max(8, int(h * scale))
    img_small = img.resize((nw, nh), Image.BICUBIC)

    gray = np.array(img_small.convert("L"), dtype=np.float32) / 255.0
    colors = np.array(img_small, dtype=np.uint8).reshape(-1, 3)

    # grid
    xs = np.linspace(-0.5, 0.5, nw, dtype=np.float32)
    zs = np.linspace(-0.5, 0.5, nh, dtype=np.float32)
    xg, zg = np.meshgrid(xs, zs)
    yg = (gray - 0.5) * 0.3  # height scale
    vertices = np.stack([xg, yg, zg], axis=-1).reshape(-1, 3)

    faces = []
    for y in range(nh - 1):
        for x in range(nw - 1):
            i0 = y * nw + x
            i1 = i0 + 1
            i2 = i0 + nw
            i3 = i2 + 1
            faces.append([i0, i2, i1])
            faces.append([i1, i2, i3])
    faces = np.asarray(faces, dtype=np.int64)

    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, vertex_colors=colors, process=False)
    out_name = f"{_uuid.uuid4()}.glb"
    out_path = os.path.join(out_dir, out_name)
    mesh.scene().export(out_path)
    return out_name


# Replicate config (Trellis)
REPLICATE_API_TOKEN = os.getenv("REPLICATE_API_TOKEN", "")
REPLICATE_MODEL = os.getenv("REPLICATE_MODEL", "")      # e.g., 'trellis-org/model-slug'
REPLICATE_VERSION = os.getenv("REPLICATE_VERSION", "")  # version hash from Replicate
if REPLICATE_API_TOKEN:
    os.environ["REPLICATE_API_TOKEN"] = REPLICATE_API_TOKEN

def _download_file(url: str, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with requests.get(url, stream=True, timeout=600) as r:
        r.raise_for_status()
        with open(dest, "wb") as f:
            for chunk in r.iter_content(1024 * 64):
                if chunk:
                    f.write(chunk)
    return dest

def _export_obj_folder_to_glb(folder: Path, out_path: Path) -> Path:
    objs = list(folder.rglob("*.obj"))
    if not objs:
        raise RuntimeError("No OBJ found in archive")
    scene = trimesh.load(str(objs[0]), force="scene")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    scene.export(str(out_path))
    return out_path

def _replicate_upload_url(client: Client, file_path: Path) -> str | None:
    """
    Upload to Replicate storage. Try client.files.upload (new SDK),
    else replicate.files.upload (older SDK). Return public URL or None.
    """
    try:
        files_attr = getattr(client, "files", None)
        upload_fn = getattr(files_attr, "upload", None) if files_attr else None
        if callable(upload_fn):
            with open(file_path, "rb") as fh:
                uploaded = upload_fn(fh)
            return getattr(uploaded, "url", None) or (uploaded if isinstance(uploaded, str) else None)
    except Exception:
        pass
    # Older SDK fallback
    try:
        up_mod = getattr(replicate, "files", None)
        upload_fn2 = getattr(up_mod, "upload", None) if up_mod else None
        if callable(upload_fn2):
            with open(file_path, "rb") as fh:
                uploaded = upload_fn2(fh)
            return getattr(uploaded, "url", None) or (uploaded if isinstance(uploaded, str) else None)
    except Exception:
        pass
    return None

# --- API ENDPOINTS ---


@app.get("/")

def read_root():

    return {"message": "Hello from the Presentation Backend!"}


@app.post("/upload-and-process")

async def process_files(files: List[UploadFile] = File(...)):

    all_slides = []

    for file in files:

        temp_filepath = os.path.join(TEMP_UPLOADS_DIR, file.filename)

        with open(temp_filepath, "wb") as buffer:

            buffer.write(await file.read())


        file_ext = os.path.splitext(file.filename)[1].lower()

        

        if file_ext == '.pdf':

            # Process PDF directly

            generated_slides = process_pdf_to_images(temp_filepath, file.filename)

            all_slides.extend(generated_slides)

        

        elif file_ext in ['.pptx', '.ppt', '.docx', '.doc', '.xlsx', '.xls']:

            # Convert Office file (including Excel) to PDF first

            pdf_path = convert_office_to_pdf(temp_filepath, TEMP_UPLOADS_DIR)

            if pdf_path and os.path.exists(pdf_path):

                # Now process the newly created PDF

                generated_slides = process_pdf_to_images(pdf_path, file.filename)

                all_slides.extend(generated_slides)

                os.remove(pdf_path) # Clean up the temporary PDF


    return {

        "success": True,

        "slides": all_slides

    }


# --- AI Slides Generation ---

class GenerateSlidesRequest(BaseModel):
    prompt: str


@app.post("/generate-ai-slides")
def generate_ai_slides(payload: GenerateSlidesRequest):
    prompt_text = (payload.prompt or "").strip()
    if not prompt_text:
        raise HTTPException(status_code=400, detail={"error": "'prompt' is required"})
    try:
        topic = prompt_text[:80]
        slides = [
            {"title": f"Introduction to {topic}", "content": f"Overview of {topic} and why it matters."},
            {"title": f"Key Concepts in {topic}", "content": f"Core ideas, terminology, and examples for {topic}."},
            {"title": f"Applications of {topic}", "content": f"Real-world use cases and next steps for {topic}."},
        ]
        return slides
    except Exception as exc:
        raise HTTPException(status_code=500, detail={"error": f"Failed to generate slides: {str(exc)}"})


# --- 3D Model Generation Endpoint ---

@app.post("/generate-3d-model")
async def generate_3d_model(image: UploadFile = File(...)):
    # Save uploaded image to temp
    img_ext = os.path.splitext(image.filename)[1]
    img_filename = f"{uuid.uuid4()}{img_ext}"
    img_path = os.path.join(TEMP_UPLOADS_DIR, img_filename)
    with open(img_path, "wb") as buffer:
        buffer.write(await image.read())
    # Generate a valid .glb from the image (heightmap)
    try:
        model_filename = make_heightmap_glb(img_path, THREED_MODELS_DIR)
    except Exception as e:
        print("3D generation failed:", e)
        raise HTTPException(status_code=500, detail="3D model generation failed.")
    return {"modelUrl": f"/static/3dmodels/{model_filename}"}


# --- Diagnostics for Replicate config ---
@app.get("/replicate/diagnostics")
def replicate_diagnostics():
    info = {
        "has_token": bool(REPLICATE_API_TOKEN),
        "model": REPLICATE_MODEL,
        "version": REPLICATE_VERSION,
    }
    try:
        if not (REPLICATE_API_TOKEN and REPLICATE_MODEL and REPLICATE_VERSION):
            return {"ok": False, "info": info, "error": "Missing token/model/version"}
        m = replicate.models.get(REPLICATE_MODEL)
        v = m.versions.get(REPLICATE_VERSION)
        return {"ok": True, "info": info, "resolved_version": v.id}
    except Exception as e:
        return {"ok": False, "info": info, "error": str(e)}

def _upload_to_transfersh(path: Path) -> str:
    # Public, ephemeral URL; suitable if the model requires image_url
    with open(path, "rb") as f:
        resp = requests.put(f"https://transfer.sh/{path.name}", data=f.read(), timeout=600)
    resp.raise_for_status()
    return resp.text.strip()

@app.post("/replicate/image-to-3d")
async def replicate_image_to_3d(
    image: UploadFile = File(...),
    texture_size: int = Form(1024),
    mesh_simplify: float = Form(0.95),
    generate_model: bool = Form(True),
    save_gaussian_ply: bool = Form(True),
    ss_sampling_steps: int = Form(38),
):
    if not REPLICATE_API_TOKEN:
        raise HTTPException(status_code=500, detail="REPLICATE_API_TOKEN not set")
    if not REPLICATE_MODEL or not REPLICATE_VERSION:
        raise HTTPException(status_code=500, detail="REPLICATE_MODEL/REPLICATE_VERSION not set")

    ext = os.path.splitext(image.filename or "")[1].lower()
    if ext not in [".jpg", ".jpeg", ".png", ".bmp", ".webp"]:
        raise HTTPException(status_code=400, detail="Unsupported image type")

    tmp_img = Path(TEMP_UPLOADS_DIR) / f"{uuid.uuid4()}{ext}"
    with open(tmp_img, "wb") as f:
        f.write(await image.read())

    try:
        client = Client(api_token=REPLICATE_API_TOKEN)
        run_id = f"{REPLICATE_MODEL}:{REPLICATE_VERSION}"

        base_inputs = {
            "texture_size": texture_size,
            "mesh_simplify": mesh_simplify,
            "generate_model": generate_model,
            "save_gaussian_ply": save_gaussian_ply,
            "ss_sampling_steps": ss_sampling_steps,
        }

        # Prefer uploading to Replicate storage (if SDK supports it)
        uploaded_url = _replicate_upload_url(client, tmp_img)

        if uploaded_url:
            inputs = {**base_inputs, "images": [uploaded_url]}
            prediction = client.predictions.create(version=run_id, input=inputs)
        else:
            # Fallback: pass the file handle; SDK uploads as part of the request
            try:
                with open(tmp_img, "rb") as fh:
                    inputs = {**base_inputs, "images": [fh]}
                    prediction = client.predictions.create(version=run_id, input=inputs)
            except Exception:
                with open(tmp_img, "rb") as fh:
                    inputs = {**base_inputs, "images": fh}
                    prediction = client.predictions.create(version=run_id, input=inputs)

        prediction.wait()
        output = prediction.output

        # Expect dict with "model_file"
        model_url_out: str | None = None
        if isinstance(output, dict):
            model_url_out = output.get("model_file") or output.get("model") or output.get("output")
        elif isinstance(output, str):
            model_url_out = output
        elif isinstance(output, list):
            model_url_out = next((u for u in output if isinstance(u, str)), None)

        if not model_url_out:
            raise RuntimeError(f"Unexpected Replicate output: {type(output)} {output!r}")

        out_name = f"{uuid.uuid4()}.glb"
        out_path = Path(THREED_MODELS_DIR) / out_name
        _download_file(model_url_out, out_path)

        return {"modelUrl": f"/static/3dmodels/{out_path.name}"}
    except Exception as e:
        traceback.print_exc()
        raise HTTPException(status_code=500, detail=f"Replicate failed: {e}")


# =========================
# NEW: AUDIO CLEAN ENDPOINT
# =========================

def _run_ffmpeg(cmd: list[str]) -> None:
    # Update this path to where your ffmpeg.exe is located
    FFMPEG_PATH = "C:\\Users\\midla\\AppData\\Local\\Microsoft\\WinGet\\Packages\\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\\ffmpeg-8.1-full_build\\bin\\ffmpeg.exe"
    try:
        proc = subprocess.run([FFMPEG_PATH, *cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    except subprocess.CalledProcessError as e:
        err = (e.stderr or b"").decode(errors="ignore")
        raise RuntimeError(f"ffmpeg failed: {err[:5000]}")

@app.post("/audio/clean")
async def clean_audio(
    audio: UploadFile = File(...),
    intensity: float = Form(0.03),     # anlmdn p param
    target_lufs: float = Form(-18.0),  # loudnorm I
):
    ext = os.path.splitext(audio.filename or "")[1].lower()
    if ext not in [".wav", ".mp3", ".m4a", ".aac", ".ogg", ".webm"]:
        raise HTTPException(status_code=400, detail="Unsupported audio type")

    tmp_in = Path(TEMP_UPLOADS_DIR) / f"{uuid.uuid4()}{ext}"
    with open(tmp_in, "wb") as f:
        f.write(await audio.read())

    out_name = f"cleaned_{uuid.uuid4().hex}.wav"
    out_path = Path(RECORDINGS_DIR) / out_name

    # wasm-safe chain but on server: good baseline denoise
    af = f"anlmdn=m=15:p={float(intensity)}:sc=1,highpass=f=80,lowpass=f=7500,loudnorm=I={float(target_lufs)}:TP=-1.5:LRA=11"
    try:
        _run_ffmpeg(["-y", "-i", str(tmp_in), "-af", af, "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(out_path)])
    except Exception as e:
        # fallback simpler
        _run_ffmpeg(["-y", "-i", str(tmp_in), "-af", f"highpass=f=100,lowpass=f=8000,loudnorm=I={float(target_lufs)}:TP=-1.5:LRA=11",
                     "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(out_path)])

    try:
        tmp_in.unlink(missing_ok=True)
    except Exception:
        pass

    return {"cleanedAudioUrl": f"/static/recordings/{out_name}"}


# =========================
# NEW: RECORDINGS SAVE/LIST
# =========================

def _load_index() -> list:
    if RECORDINGS_INDEX.exists():
        try:
            return json.load(open(RECORDINGS_INDEX, "r", encoding="utf-8"))
        except Exception:
            return []
    return []

def _save_index(items: list) -> None:
    with open(RECORDINGS_INDEX, "w", encoding="utf-8") as f:
        json.dump(items, f, ensure_ascii=False, indent=2)

def _ffprobe_duration_seconds(path: Path) -> float:
    try:
        out = subprocess.check_output([
            "ffprobe","-v","error","-show_entries","format=duration","-of","default=noprint_wrappers=1:nokey=1",str(path)
        ], stderr=subprocess.STDOUT).decode().strip()
        return float(out)
    except Exception:
        return 0.0

@app.post("/recordings/save")
async def save_recording(
    title: str = Form("Untitled Video"),
    offset_ms: int = Form(0),
    video: UploadFile = File(...),
    audio: UploadFile | None = File(None),
):
    # Save inputs to temp
    vext = os.path.splitext(video.filename or "")[1].lower() or ".webm"
    aext = os.path.splitext((audio.filename if audio else "") or "")[1].lower() if audio else ".wav"

    vtmp = Path(TEMP_UPLOADS_DIR) / f"v_{uuid.uuid4().hex}{vext}"
    with open(vtmp, "wb") as f:
        f.write(await video.read())

    if audio:
        atmp = Path(TEMP_UPLOADS_DIR) / f"a_{uuid.uuid4().hex}{aext}"
        with open(atmp, "wb") as f:
            f.write(await audio.read())
    else:
        # extract audio from video if none provided
        atmp = Path(TEMP_UPLOADS_DIR) / f"a_{uuid.uuid4().hex}.wav"
        try:
            _run_ffmpeg(["-y","-i",str(vtmp),"-vn","-ac","2","-ar","48000","-c:a","pcm_s16le",str(atmp)])
        except Exception as e:
            vtmp.unlink(missing_ok=True)
            raise HTTPException(status_code=400, detail=f"Failed to extract audio: {e}")

    # Merge with offset and transcode to MP4 (H.264), fallback to mpeg4
    rec_id = uuid.uuid4().hex
    out_mp4 = Path(RECORDINGS_DIR) / f"{rec_id}.mp4"
    out_poster = Path(RECORDINGS_DIR) / f"{rec_id}.jpg"

    duration = _ffprobe_duration_seconds(vtmp)
    off = abs(int(offset_ms)) / 1000.0
    inputs = []
    if offset_ms > 0:
        inputs = ["-y","-i",str(vtmp),"-itsoffset",f"{off:.3f}","-i",str(atmp)]
    elif offset_ms < 0:
        inputs = ["-y","-ss",f"{off:.3f}","-i",str(atmp),"-i",str(vtmp)]
    else:
        inputs = ["-y","-i",str(vtmp),"-i",str(atmp)]

    # Try libx264
    try:
        _run_ffmpeg(inputs + [
            "-c:v","libx264","-preset","medium","-crf","18","-pix_fmt","yuv420p",
            "-c:a","aac","-b:a","192k","-movflags","+faststart","-shortest",str(out_mp4)
        ])
    except Exception:
        # fallback mpeg4
        _run_ffmpeg(inputs + [
            "-c:v","mpeg4","-qscale:v","2","-pix_fmt","yuv420p",
            "-c:a","aac","-b:a","192k","-shortest",str(out_mp4)
        ])

    # Poster (midpoint frame)
    mid = max(0.0, (duration or 1.0) / 2.0)
    try:
        _run_ffmpeg(["-y","-ss",f"{mid:.2f}","-i",str(out_mp4),"-frames:v","1","-vf","scale=1280:-1","-q:v","2",str(out_poster)])
    except Exception:
        pass

    # Cleanup temps
    try:
        vtmp.unlink(missing_ok=True)
        atmp.unlink(missing_ok=True)
    except Exception:
        pass

    size_bytes = out_mp4.stat().st_size if out_mp4.exists() else None
    item = {
        "id": rec_id,
        "title": title,
        "offset_ms": int(offset_ms),
        "mp4Url": f"/static/recordings/{out_mp4.name}",
        "thumbUrl": f"/static/recordings/{out_poster.name}" if out_poster.exists() else None,
        "durationSec": duration,
        "sizeBytes": size_bytes,
        "savedAt": Path(out_mp4).stat().st_mtime
    }
    index = _load_index()
    index.insert(0, item)
    _save_index(index)

    return item

@app.get("/recordings")
def list_recordings():
    return _load_index()

@app.get("/recordings/{rec_id}")
def get_recording(rec_id: str):
    for it in _load_index():
        if it.get("id") == rec_id:
            return it
    raise HTTPException(status_code=404, detail="Recording not found")

@app.delete("/recordings/{rec_id}")
def delete_recording(rec_id: str):
    index = _load_index()
    item = next((x for x in index if x.get("id") == rec_id), None)
    if not item:
        raise HTTPException(status_code=404, detail="Recording not found")
    # delete files
    try:
        mp4 = Path(RECORDINGS_DIR) / f"{rec_id}.mp4"
        jpg = Path(RECORDINGS_DIR) / f"{rec_id}.jpg"
        if mp4.exists(): mp4.unlink()
        if jpg.exists(): jpg.unlink()
    except Exception:
        pass
    # update index
    index = [x for x in index if x.get("id") != rec_id]
    _save_index(index)
    return {"ok": True}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)

# .venv\Scripts\Activate.ps1
# uvicorn main:app --reload