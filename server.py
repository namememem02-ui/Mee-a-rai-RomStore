import os
import sys
import json
import shutil
import socket
import re
from pathlib import Path
from bottle import Bottle, request, response, static_file, run

try:
    from PIL import Image
except ImportError:
    Image = None

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if sys.stderr and hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

app = Bottle()

BASE_DIR = Path(__file__).resolve().parent
STORAGE_DIR = BASE_DIR / "storage"
ROMS_DIR = STORAGE_DIR / "Roms"

SYSTEM_MAPPING = {
    "GBA": "Game Boy Advance (GBA)",
    "PSX": "Sony PlayStation (PS)",
    "PSP": "Sony PlayStation Portable (PSP)",
    "NDS": "Nintendo DS (NDS)",
    "FC": "Nintendo Entertainment System (FC)",
    "SFC": "Super Nintendo Entertainment System (SFC)",
    "MD": "Sega Genesis (MD)",
    "GB": "Game Boy (GB)",
    "GBC": "Game Boy Color (GBC)"
}

EXT_SYSTEM_MAP = {
    ".gba": "Game Boy Advance (GBA)",
    ".chd": "Sony PlayStation (PS)",
    ".iso": "Sony PlayStation Portable (PSP)",
    ".pbp": "Sony PlayStation (PS)",
    ".nds": "Nintendo DS (NDS)",
    ".nes": "Nintendo Entertainment System (FC)",
    ".sfc": "Super Nintendo Entertainment System (SFC)",
    ".smc": "Super Nintendo Entertainment System (SFC)",
    ".md": "Sega Genesis (MD)",
    ".bin": "Sony PlayStation (PS)",
    ".zip": "Game Boy Advance (GBA)"
}

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

def init_storage():
    """Ensure all console directories and .media folders exist within storage/Roms"""
    for sys_folder in SYSTEM_MAPPING.values():
        sys_path = ROMS_DIR / sys_folder
        media_path = sys_path / ".media"
        sys_path.mkdir(parents=True, exist_ok=True)
        media_path.mkdir(parents=True, exist_ok=True)

def check_is_thai(filename):
    name = filename.lower()
    # Explicit Thai keywords
    if any(k in name for k in ['thai', 'แปลไทย', 'nonplae', 'thaimod', 'auns555', 'memorycardth']):
        return True
    # Word boundary check for 'th' (not substring like Brea[th])
    if re.search(r'[\(\[\_\-\s]th[\)\]\_\-\s\.]', name) or re.search(r'[\(\[\_\-\s]th$', name):
        return True
    return False

@app.route('/')
def index():
    return static_file('index.html', root=str(BASE_DIR / 'static'))

@app.route('/static/<filepath:path>')
def server_static(filepath):
    return static_file(filepath, root=str(BASE_DIR / 'static'))

@app.route('/api/info')
def api_info():
    return {
        "server_ip": get_local_ip(),
        "port": 8080,
        "supported_systems": SYSTEM_MAPPING
    }

@app.route('/api/catalog')
def api_catalog():
    catalog = []
    local_ip = get_local_ip()
    base_url = f"http://{local_ip}:8080"
    
    for sys_key, full_sys_name in SYSTEM_MAPPING.items():
        sys_path = ROMS_DIR / full_sys_name
        media_path = sys_path / ".media"
        if not sys_path.exists():
            continue
            
        for f in sys_path.iterdir():
            if f.is_file() and not f.name.startswith('.'):
                stem = f.stem
                cover_file = media_path / f"{stem}.png"
                has_cover = cover_file.exists()
                size_mb = round(f.stat().st_size / (1024 * 1024), 2)
                
                is_thai = check_is_thai(f.name)
                
                catalog.append({
                    "id": f"{full_sys_name}__{f.name}",
                    "name": stem,
                    "filename": f.name,
                    "system": full_sys_name,
                    "system_short": sys_key,
                    "size_mb": size_mb,
                    "size_bytes": f.stat().st_size,
                    "is_thai": is_thai,
                    "category": "แปลไทย" if is_thai else "ต้นฉบับ",
                    "badge_text": "🇹🇭 แปลไทย" if is_thai else "📦 ต้นฉบับ",
                    "has_cover": has_cover,
                    "rom_url": f"{base_url}/api/download/rom/{full_sys_name}/{f.name}",
                    "cover_url": f"{base_url}/api/download/cover/{full_sys_name}/{stem}.png" if has_cover else None
                })
                
    catalog.sort(key=lambda x: (not x['is_thai'], x['system'], x['name']))
    return {"games": catalog, "total": len(catalog)}

@app.route('/api/catalog/sync')
def api_catalog_sync():
    """Text-based endpoint formatted specifically for TrimUI Smart Pro shell scripts"""
    lines = []
    local_ip = get_local_ip()
    base_url = f"http://{local_ip}:8080"
    import urllib.parse
    
    for sys_key, full_sys_name in SYSTEM_MAPPING.items():
        sys_path = ROMS_DIR / full_sys_name
        media_path = sys_path / ".media"
        if not sys_path.exists():
            continue
            
        for f in sys_path.iterdir():
            if f.is_file() and not f.name.startswith('.'):
                stem = f.stem
                cover_file = media_path / f"{stem}.png"
                has_cover = "1" if cover_file.exists() else "0"
                encoded_sys = urllib.parse.quote(full_sys_name)
                encoded_file = urllib.parse.quote(f.name)
                encoded_stem = urllib.parse.quote(stem)
                
                rom_url = f"{base_url}/api/download/rom/{encoded_sys}/{encoded_file}"
                cover_url = f"{base_url}/api/download/cover/{encoded_sys}/{encoded_stem}.png" if cover_file.exists() else ""
                try:
                    size_mb = f"{f.stat().st_size / (1024 * 1024):.1f} MB"
                except Exception:
                    size_mb = "N/A"
                is_thai = "1" if check_is_thai(f.name) else "0"
                lines.append(f"{full_sys_name}|{f.name}|{stem}|{has_cover}|{rom_url}|{cover_url}|{size_mb}|{is_thai}")
                
    response.content_type = 'text/plain; charset=utf-8'
    return "\n".join(lines)

@app.route('/api/download/rom/<system>/<filename:path>')
def download_rom(system, filename):
    return static_file(filename, root=str(ROMS_DIR / system), download=True)

@app.route('/api/download/cover/<system>/<filename:path>')
def download_cover(system, filename):
    return static_file(filename, root=str(ROMS_DIR / system / ".media"))

@app.route('/api/upload', method='POST')
def api_upload():
    try:
        rom_file = request.files.get('rom')
        cover_file = request.files.get('cover')
        chosen_system = request.forms.get('system')
        custom_name = request.forms.get('custom_name', '').strip()
        
        if not rom_file:
            response.status = 400
            return {"error": "กรุณาเลือกหรือโยนไฟล์ ROM"}
            
        ext = Path(rom_file.filename).suffix.lower()
        if not chosen_system or chosen_system not in SYSTEM_MAPPING.values():
            if chosen_system in SYSTEM_MAPPING:
                chosen_system = SYSTEM_MAPPING[chosen_system]
            else:
                chosen_system = EXT_SYSTEM_MAP.get(ext, "Game Boy Advance (GBA)")
                
        target_sys_dir = ROMS_DIR / chosen_system
        target_media_dir = target_sys_dir / ".media"
        target_sys_dir.mkdir(parents=True, exist_ok=True)
        target_media_dir.mkdir(parents=True, exist_ok=True)
        
        stem = custom_name if custom_name else Path(rom_file.filename).stem
        final_rom_name = f"{stem}{ext}"
        final_rom_path = target_sys_dir / final_rom_name
        
        rom_file.save(str(final_rom_path), overwrite=True)
        
        if cover_file:
            final_cover_path = target_media_dir / f"{stem}.png"
            if Image:
                try:
                    img = Image.open(cover_file.file)
                    img = img.convert("RGBA")
                    resample_filter = getattr(Image, 'Resampling', Image).LANCZOS
                    img.thumbnail((500, 500), resample_filter)
                    img.save(final_cover_path, "PNG", optimize=True)
                except Exception:
                    cover_file.save(str(final_cover_path), overwrite=True)
            else:
                cover_file.save(str(final_cover_path), overwrite=True)
            
        return {
            "success": True,
            "message": f"บันทึกเกม {stem} ({chosen_system}) สำเร็จ พร้อมให้ TrimUI ดาวน์โหลดทันที!",
            "rom": final_rom_name
        }
    except Exception as e:
        response.status = 500
        return {"error": str(e)}

@app.route('/api/delete', method='POST')
def api_delete():
    try:
        system = request.forms.get('system')
        filename = request.forms.get('filename')
        if not system or not filename:
            response.status = 400
            return {"error": "Missing system or filename"}
            
        rom_path = ROMS_DIR / system / filename
        cover_path = ROMS_DIR / system / ".media" / f"{Path(filename).stem}.png"
        
        if rom_path.exists():
            rom_path.unlink()
        if cover_path.exists():
            cover_path.unlink()
            
        return {"success": True, "message": f"ลบ {filename} เรียบร้อยแล้ว"}
    except Exception as e:
        response.status = 500
        return {"error": str(e)}

PAK_DIR = BASE_DIR / "client_pak" / "ThaiRomStore.pak"

@app.route('/api/app/version')
def api_app_version():
    pak_json_file = PAK_DIR / "pak.json"
    ver_str = "1.2.0"
    if pak_json_file.exists():
        try:
            with open(pak_json_file, 'r', encoding='utf-8') as pf:
                data = json.load(pf)
                ver_str = data.get("version", ver_str).lstrip("v")
        except Exception:
            pass
            
    elf_file = PAK_DIR / "bin" / "store.elf"
    build_time = ""
    if elf_file.exists():
        import datetime
        mtime = elf_file.stat().st_mtime
        build_time = datetime.datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M:%S")

    return {
        "version": ver_str,
        "build_time": build_time,
        "changelog": "แยกตัวกรองเครื่องเกม PSX และ PSP ชัดเจน 100% ไม่ปนกัน พร้อมแสดงชื่อเครื่องบนปุ่มกรอง"
    }

@app.route('/api/app/latest/store.elf')
def api_latest_store_elf():
    return static_file("store.elf", root=str(PAK_DIR / "bin"), download=True)

@app.route('/api/app/latest/launch.sh')
def api_latest_launch_sh():
    return static_file("launch.sh", root=str(PAK_DIR), download=True)

@app.route('/api/app/latest/autoscan.sh')
def api_latest_autoscan_sh():
    return static_file("autoscan.sh", root=str(PAK_DIR / "bin"), download=True)

if __name__ == '__main__':
    print("=" * 60)
    print("🚀 Initializing Mee-a-rai ROM Store...")
    init_storage()
    ip = get_local_ip()
    port = 8080
    print(f"✨ Mee-a-rai ROM Store Server running on:")
    print(f"   💻 PC Web Dashboard: http://localhost:{port}")
    print(f"   📶 TrimUI Smart Pro Wi-Fi IP: http://{ip}:{port}")
    print("=" * 60)
    run(app, host='0.0.0.0', port=port, debug=False)
