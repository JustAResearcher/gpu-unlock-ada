# GPU unlock 1.1.1 — RTX 4090 y GPUs Ada experimentales

¿Nuevo en este proyecto? Comienza con la [Guía de Inicio Rápido (QUICKSTART)](QUICKSTART.md) · **[Mejoras del fork](#mejoras-del-fork-edición-dannybaanks--isyco)** · **[English version](README.md)**

> [!NOTE]
> **Información de Línea Base y Divergencia del Fork (DannyBaanks / ISyCo)**
>
> Este repositorio es un fork especializado de investigación y desarrollo mantenido por DannyBaanks y el grupo de investigación ISyCo (`https://github.com/DannyBaanks/gpu-unlock-ada`).
>
> - **Repositorio Oficial (Upstream)**: [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada)
> - **Commit Base de Upstream**: `d28c485038cbbab2ebfa62ffeb5c179c37e6da48` ("bench: accept N/A as the expected power limit for laptops")
> - **Pull Request Activo**: [PR #134: feat(ad107): admit 10de:28a1 canary (RTX 4050 Laptop) and document Optimus D3cold power-rail trap](https://github.com/JustAResearcher/gpu-unlock-ada/pull/134)
> - **Rama Canaria Activa**: `canary-28a1-ad107` (4 commits por delante de upstream main)
> - **Mejoras Clave**:
>   1. **Calificación Canaria AD107M (`10de:28a1`)**: Admisión de la GPU RTX 4050 Laptop bajo `ad107_experimental`, superando la transacción de firmware de 4 PLMs, el handoff PCIe FLR y la vinculación del driver sin errores Xid.
>   2. **Descubrimiento de la Trampa de Riel `D3cold` en Optimus**: Descubrimiento del reseteo silencioso de hardware donde el modo D3cold en portátiles híbridos corta la energía de la dGPU mientras `NVreg_PreserveVideoMemoryAllocations=1` provoca que el getter de GSP-RM reporte lecturas cacheadas falsas-positivas `0 0 0`.
>   3. **Ejecución Tensorial Persistente a 2.00× (`DynamicPowerManagement=0x00`)**: Solución a la trampa de caída de riel sin bloquear la GPU en estados de alta potencia, demostrando una tasa de emisión MMA de **2.002×** (`f16_f32` 25.32 → 50.68 Top/s) y mejoras en cuBLAS de +36.2% a +61.6%.
>   4. **Integración con el Ecosistema Tri-Repo**: Interoperabilidad nativa con el [Hardware Research Agent (IHRA)](https://github.com/DannyBaanks/hardware-research-agent) y el [Motor Forense REA](https://github.com/DannyBaanks/rea).

---

Este módulo del kernel de Linux activa la anulación de selección de velocidad de SM antes de que inicie el controlador oficial de NVIDIA. El soporte para la RTX 4090 (`10de:2684`, AD102) ha demostrado aproximadamente **el doble de throughput tensorial FP16 y BF16 con acumulación FP32**:

| Modo | Antes | Después |
|---|---:|---:|
| Acumulación FP16 → FP32 | 165.6 TF/s | 331.6 TF/s |
| Acumulación BF16 → FP32 | 165.5 TF/s | 330.6 TF/s |

La anulación sobrevive al Restablecimiento a Nivel de Función PCIe (FLR), a la inicialización del controlador estándar de NVIDIA y a los benchmarks de CUDA. INT8, `dp4a` y FP32 escalar se mantuvieron cerca de sus tasas base.

---

## Mejoras del fork: Edición DannyBaanks / ISyCo

Este fork contiene validación experimental, descubrimientos de arquitectura y calificaciones canarias desarrolladas por DannyBaanks y el grupo de investigación ISyCo.

### 1. Calificación canaria móvil AD107M (`10de:28a1`)

Valida el payload completo de cuatro PLMs en la arquitectura móvil Ada Lovelace AD107M:
- **Plataforma de prueba**: HP Victus 15-fb3xxx (AMD Ryzen 5 8645HS con gráficos Radeon 760M, GPU RTX 4050 Laptop `10de:28a1`, 20 SMs, rev a1, subsistema `103c:8dd0`).
- **Entorno de software**: Ubuntu 24.04 LTS (kernel Linux `7.0.0-42-generic`), módulos de kernel abiertos NVIDIA `580.178.04` (`nvidia-driver-580-open`).
- **Integridad transaccional**: El dispositivo se admite de forma limpia bajo `ad107_experimental`, sobrevive al handoff PCIe FLR hacia el controlador abierto estándar con cero errores Xid, y supera todas las pruebas de estabilidad matemática y del controlador.

### 2. La trampa del riel de energía `D3cold` en Optimus y su resolución

En portátiles híbridos/Optimus donde la sesión de escritorio es gestionada por la GPU integrada (AMD Radeon 760M vía `amdgpu` o Intel i915/xe), la administración estándar de energía en tiempo de ejecución de Linux (`NVreg_DynamicPowerManagement=0x02`) suspende la GPU dedicada NVIDIA a `D3cold` (`Video Memory: Off`, ACPI `LNXPOWER:00=0`) pocos segundos tras arrancar.

#### La trampa del reseteo silencioso de hardware
1. Cuando la dGPU entra en `D3cold`, se corta la alimentación física al chip de la GPU.
2. Al reanudar de nuevo a `D0`, los registros volátiles de anulación de velocidad (`0x00823804`, `0x0082381c`, `0x00823820`) se restablecen a los fusibles de media velocidad de fábrica (`NV_PFUSE_FEATURE_READOUT_1 = 0x00410080`).
3. **La trampa del falso positivo**: Debido a que `NVreg_PreserveVideoMemoryAllocations=1` restaura el estado `OBJGR` cacheado de GSP-RM entre ciclos de energía sin volver a leer el registro físico de fusibles, la consulta estándar del getter de RM (`tools/read_sm_rates.py`, ioctl `0x20801230`) continúa devolviendo `0 0 0` (velocidad completa). El software reporta una tarjeta desbloqueada mientras el silicio físico ejecuta a la velocidad de fábrica de 1.00×.

#### La resolución canónica (`NVreg_DynamicPowerManagement=0x00`)
Para retener el desbloqueo de hardware de forma persistente preservando la acústica y batería del portátil:
```ini
# /etc/modprobe.d/nvidia-gpu-unlock-nod3cold.conf
options nvidia NVreg_DynamicPowerManagement=0x00
```
- **Por qué funciona**: Establecer `DynamicPowerManagement=0x00` desactiva el D3cold en tiempo de ejecución dentro del controlador NVIDIA mientras permite que GSP-RM baje a **P8 idle** (210 MHz core / ~1.6 W / ventiladores apagados).
- **Por qué forzar PCI sysfs falla**: Forzar `power/control="on"` a nivel de sysfs PCI bloquea GSP-RM en **P3** (1755 MHz / ~9.7 W) y genera advertencias espurias `GPS ACPI DSM` / `NV_ERR_NO_MEMORY` cada vez que clientes NVML abren `/dev/nvidia0`.

### 3. Tasa de emisión tensorial sostenida a 2.00× y triangulación de telemetría

Al inhibir `D3cold`, el chip AD107M entrega una tasa de emisión tensorial sostenida de **2.00×** en todos los modos de precisión evaluados sin degradación térmica ni numérica:

| Precisión / Modo | Stock (Caída de riel D3cold) | Desbloqueado (`DynamicPowerManagement=0x00`) | Ratio Medido |
|---|---:|---:|---:|
| **MMA `f16_f32`** | 25.320 Top/s | **50.682 Top/s** | **2.002×** |
| **MMA `tf32`** | 12.660 Top/s | **25.281 Top/s** | **1.997×** |
| **`mma_bench` `f16/f32`** | 25.5 TF/s | **50.9 TF/s** | **1.996×** |
| **`mma_bench` `bf16/f32`** | 25.5 TF/s | **50.9 TF/s** | **1.996×** |
| **`mma_bench` `fp8/e4m3`** | 50.9 TF/s | **101.4 TF/s** | **1.992×** |
| **cuBLAS $4096^3$ GEMM FP16** | 19.82 TF/s | **27.00 TF/s** | **+36.2%** |
| **cuBLAS $4096^3$ GEMM BF16** | 19.85 TF/s | **32.08 TF/s** | **+61.6%** |

- **Paridad Numérica**: 24/24 geometrías GEMM con semilla en FP16 y BF16 (`256×256×256`, `64×96×128`, `127×65×95`) coinciden bit a bit con la referencia CPU FP64 (`failures=0`, `max_abs=0`).
- **Recuperación en Caliente sin Reinicio**: Se documentó el restablecimiento en frío mediante transiciones ACPI `D3cold` → `D0` sin necesidad de reiniciar la máquina ni restablecer el controlador embebido (EC).

### 4. Puente del ecosistema tri-repo (IHRA + REA + Ada Unlock)

Este fork actúa como el pilar de aceleración por GPU en el conjunto de herramientas de investigación de ISyCo:
- **[Hardware Research Agent (IHRA)](https://github.com/DannyBaanks/hardware-research-agent)**: Proporciona telemetría automatizada y validación canaria mediante un CLI unificado sin bloat (`ihra ada` / `./bin/ihra.mjs ada`).
- **[Herramientas Forenses REA](https://github.com/DannyBaanks/rea)**: Proporciona análisis estático de binarios y un perfil compacto de divulgación progresiva (`REA_MCP_TOOL_PROFILE=compact`), eliminando la saturación de tokens en MCP.

### 5. Tabla de sincronización y línea base

| Propiedad | Valor |
| --- | --- |
| Repositorio Oficial (Upstream) | [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) |
| Repositorio del Fork | [DannyBaanks/gpu-unlock-ada](https://github.com/DannyBaanks/gpu-unlock-ada) |
| Commit Base de Upstream | `d28c485038cbbab2ebfa62ffeb5c179c37e6da48` (31 de octubre de 2026) |
| Pull Request Oficial Activo | [PR #134](https://github.com/JustAResearcher/gpu-unlock-ada/pull/134) |
| Rama Canaria Activa | `canary-28a1-ad107` |
| Commits por Delante de Upstream | 4 commits (`9d42480`, `0526169`, `12ec500`, `6e1035c`) |
| Estado de la Suite de Pruebas | 33/33 pruebas en verde (`make test`) |

---

## Compilación e instalación

Clona el código bajo licencia GPL-2.0 y compila los paquetes localmente:

```sh
git clone https://github.com/DannyBaanks/gpu-unlock-ada.git
cd gpu-unlock-ada
git checkout canary-28a1-ad107
make install
```

`make install` detecta automáticamente la familia de distribución. Los objetivos explícitos son:

```sh
make install-arch   # makepkg -sif
make install-deb    # apt-get build-dep, dpkg-buildpackage, apt-get install
make install-rpm    # dnf builddep, rpmbuild, dnf install
```

Configura el parámetro del kernel en tu cargador de arranque:
```text
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1
```

Añade la persistencia energética contra la caída de riel en portátiles híbridos:
```sh
echo "options nvidia NVreg_DynamicPowerManagement=0x00" | sudo tee /etc/modprobe.d/nvidia-gpu-unlock-nod3cold.conf
sudo update-initramfs -u
```

---

## Verificación y diagnóstico

Inspecciona el log del kernel tras arrancar:

```sh
journalctl -k -b | grep gpu_unlock
lspci -nnk -s 0000:01:00.0
```

Para verificar que el riel de energía no ha colapsado y que el multiplicador está activo, utiliza el comprobador de modos tensoriales:

```sh
python3 diagnostics/bench/tensor_modes_driver_runner.py
```
O mediante el agente de investigación de hardware:
```sh
npx isyco-hw ada
```

---

## Créditos y licencias

El módulo de Linux y el empaquetado derivan de [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock). El payload de cuatro PLMs para AD102 deriva de [TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker). El kernel y scripts están licenciados bajo GPL-2.0. El firmware de NVIDIA permanece bajo sus propios términos.
