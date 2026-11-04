# GPU unlock 1.1.1 — RTX 4090 y GPUs Ada experimentales

**Fork de investigación DannyBaanks / ISyCo** · [Proyecto original](https://github.com/JustAResearcher/gpu-unlock-ada) · [PR #134](https://github.com/JustAResearcher/gpu-unlock-ada/pull/134) · [English](README.md)

> [!WARNING]
> **Investigación experimental de kernel y firmware, no un optimizador universal.** Los datos proceden de una sola HP Victus RTX 4050 Laptop. Cambiar módulos, parámetros de arranque o administración de energía puede afectar pantalla, suspensión o inicio. Lee el [informe](docs/ada-28a1-report.md) y [sus límites](docs/evidence/ada28a1/README.md) antes de intervenir. **Ningún 2× está garantizado en otros equipos o aplicaciones.**

### RTX 4050 Laptop (AD107M) — resultados comprobables

| Prueba | Stock | Desbloqueo retenido | Resultado |
|---|---:|---:|---:|
| Instrucciones tensoriales FP16/FP32, **mejor** TOP/s | 25.320 | **50.682** | **2.002×** |
| Instrucciones TF32, **mejor** TOP/s | 12.660 | **25.281** | **1.997×** |
| Control INT4, **mejor** TOP/s | 201.30 | 201.84 | **1.003×** (sin cambio) |
| cuBLAS 4096³ FP16, **mediana** TF/s | 22.47 | **30.54** | **+35.9 %** |
| cuBLAS 4096³ BF16, **mediana** TF/s | 21.48 | **31.85** | **+48.3 %** |

El 2× corresponde a una **microprueba de instrucciones**, no al rendimiento general de aplicaciones. Una de las tres repeticiones FP16/FP32 desbloqueadas dio **42.73 TOP/s** frente a otras dos próximas a 50.6. El mecanismo exacto de caché interna de GSP-RM es una **explicación inferida**, aunque la discrepancia entre getter, registros BAR0 y rendimiento sí está documentada.

**Hallazgo clave:** después de `D3cold` el controlador podía seguir indicando un desbloqueo, aunque **los registros físicos y el rendimiento regresaron al estado stock**. La lectura del software no basta para demostrar el estado del silicio.

[Guía original](QUICKSTART.md) · [Hallazgos del fork](#mejoras-del-fork-edición-dannybaanks--isyco) · [Datos y hashes](docs/evidence/ada28a1/README.md)

> [!NOTE]
> Este fork deriva de [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada), base [`d28c485`](https://github.com/JustAResearcher/gpu-unlock-ada/commit/d28c485038cbbab2ebfa62ffeb5c179c37e6da48). La admisión AD107M se propone en la [PR #134](https://github.com/JustAResearcher/gpu-unlock-ada/pull/134); no es una versión estable oficial del upstream.

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

### 3. Medición tensorial y cuBLAS: alcance y límites

En las pruebas capturadas, las **mejores** muestras de emisión de instrucciones rozan el 2×. Los GEMM cuBLAS presentan ganancias menores y dependientes del tamaño; no hay evidencia de estabilidad térmica indefinida:

| Precisión / Modo | Stock (Caída de riel D3cold) | Desbloqueado (`DynamicPowerManagement=0x00`) | Ratio Medido |
|---|---:|---:|---:|
| **MMA `f16_f32`** | 25.320 Top/s | **50.682 Top/s** | **2.002×** |
| **MMA `tf32`** | 12.660 Top/s | **25.281 Top/s** | **1.997×** |
| **`mma_bench` `f16/f32`** | 25.5 TF/s | **50.9 TF/s** | **1.996×** |
| **`mma_bench` `bf16/f32`** | 25.5 TF/s | **50.9 TF/s** | **1.996×** |
| **`mma_bench` `fp8/e4m3`** | 50.9 TF/s | **101.4 TF/s** | **1.992×** |
| **cuBLAS $4096^3$ GEMM FP16, mediana** | 22.47 TF/s | **30.54 TF/s** | **+35.9%** |
| **cuBLAS $4096^3$ GEMM BF16, mediana** | 21.48 TF/s | **31.85 TF/s** | **+48.3%** |

- **Verificación numérica:** los casos GEMM con semilla registran `failures=0` y `max_abs=0` frente a CPU FP64. No demuestra equivalencia bit a bit universal.
- **Alcance:** **+61.6 %** BF16 es una comparación *mejor contra mejor*; la mejora de la **mediana** es **+48.3 %**. El 2× de las pruebas de instrucciones tampoco es un mínimo sostenido. Los controles negativos con `D3cold` no fueron neutrales en cuBLAS: quedaron entre 13 % y 19 % *por debajo* de stock en $4096^3$/$8192^3$ (causa no medida).
- **Recuperación en Caliente sin Reinicio**: Se documentó el restablecimiento en frío mediante transiciones ACPI `D3cold` → `D0` sin necesidad de reiniciar la máquina ni restablecer el controlador embebido (EC).

### 4. Proyectos relacionados (sin integración automática demostrada)

Estos repositorios pueden complementar la investigación. Su existencia **no prueba integración nativa extremo a extremo** con este módulo:
- **[Hardware Research Agent (IHRA)](https://github.com/DannyBaanks/hardware-research-agent)**: Herramientas de investigación y telemetría; instalación y validación independientes.
- **[Herramientas Forenses REA](https://github.com/DannyBaanks/rea)**: Ingeniería inversa y descubrimiento progresivo de herramientas MCP. Su ahorro de catálogo no implica mejoras directas de GPU.

### 5. Tabla de sincronización y línea base

| Propiedad | Valor |
| --- | --- |
| Repositorio Oficial (Upstream) | [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) |
| Repositorio del Fork | [DannyBaanks/gpu-unlock-ada](https://github.com/DannyBaanks/gpu-unlock-ada) |
| Commit Base de Upstream | `d28c485038cbbab2ebfa62ffeb5c179c37e6da48` (31 de octubre de 2026) |
| Pull Request Oficial Activo | [PR #134](https://github.com/JustAResearcher/gpu-unlock-ada/pull/134) |
| Rama Canaria Activa | `canary-28a1-ad107` |
| Diferencias respecto al upstream | Consultar la [comparación actual](https://github.com/JustAResearcher/gpu-unlock-ada/compare/main...DannyBaanks:gpu-unlock-ada:canary-28a1-ad107), sin fijar un conteo que envejece |
| Suite reportada | 33 pruebas aprobadas; 3 pruebas opcionales PyCryptodome omitidas; validación física limitada a una laptop |

---

 > [!CAUTION]
> **Advertencia:** instalar un módulo del kernel, cambiar `modprobe` o reconstruir `initramfs` puede impedir el arranque o afectar la dGPU. Las instrucciones siguientes son experimentales, no una optimización recomendada para otros equipos. Revisa [riesgos y recuperación](docs/ada-28a1-report.md) y conserva un arranque funcional. Nunca uses solo el getter del driver para declarar el desbloqueo.

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
Para usar herramientas de investigación externas como IHRA, consulta su repositorio y verifica los comandos reales de su versión antes de instalarlas. No hay aquí una integración automática verificada.

---

## Créditos y licencias

El módulo de Linux y el empaquetado derivan de [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock). El payload de cuatro PLMs para AD102 deriva de [TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker). El kernel y scripts están licenciados bajo GPL-2.0. El firmware de NVIDIA permanece bajo sus propios términos.
