# Утилиты портирования и дампа (Porting & Diagnostic Toolkit)

В этой папке собраны скрипты, использованные для анализа VBIOS, расчёта смещений и верификации разблокировки на связке **Gigabyte GeForce RTX 4070 Ti + Huananzhi X99**.

---

### 1. `dump_vbios.bat`
* **Назначение**: Снятие безопасного дампа VBIOS (SPI EEPROM) из-под Windows в режиме только для чтения.
* **Использование**: Запустить от имени Администратора. Скрипт вызывает `nvflashk.exe --save vbios_dump.rom` и сразу передаёт дамп в `parse_vbios_fwsec.py`.

---

### 2. `parse_vbios_fwsec.py`
* **Назначение**:
  - Автоматически находит смещение VBIOS (0x55AA + PCIR) даже в полных 2 МБ SPI дампах.
  - Анализирует цепочку PCI Expansion ROM.
  - Находит заголовок `BIT`, таблицу микрокодов (`ucode table`) и дескриптор `FWSEC 0x85`.
  - Извлекает точные смещения и геометрию Falcon/SEC2 (`image_off`, `image_size`, `imem_size`, `pkc_off`, `sig_bytes` и т.д.).
  - Вычисляет точный размер чистого VBIOS Slot 0 (`626 176 байт`) и хеш 1 МБ PROM MMIO апертуры (`BAR0 + 0x300000`).
  - **Генерирует готовый C-код для `profile.c`**.
* **Использование**:
  ```bash
  python parse_vbios_fwsec.py <имя_файла.rom>
  ```

---

### 3. `extract_assets.py`
* **Назначение**: Проверка контрольных сумм SHA-256 и размеров 5 бинарников прошивки AD102 в папке `AD104-ASSETS/`.
* **Использование**:
  ```bash
  python extract_assets.py
  # Или сразу с копированием на флешку:
  python extract_assets.py --copy-to G:/AD104-ASSETS
  ```

---

### 4. `benchmark_bf16.py`
* **Назначение**: Замер реальной производительности матрицы умножения GEMM (BF16, FP16, FP32) через PyTorch CUDA, частоты ядер и энергопотребления (через `nvidia-smi`).
* **Использование**:
  ```bash
  python benchmark_bf16.py
  ```
