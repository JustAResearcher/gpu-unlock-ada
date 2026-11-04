# Diagnóstico D3cold — qué hacer (2 reinicios, ~5 min cada uno)

No se modifica ningún archivo del sistema: solo se edita la línea de arranque UNA vez en GRUB.
Si algo sale raro, reinicia normal y todo vuelve a como está hoy.

## Reinicio 1 — "wiped" (reproducir las corridas lentas)
1. Reinicia. En el menú de GRUB, con Ubuntu seleccionado, presiona `e`.
2. En la línea que empieza con `linux`, cambia `nvidia.NVreg_DynamicPowerManagement=0x00` por `nvidia.NVreg_DynamicPowerManagement=0x02`.
   (Deja `gpu_unlock.targets=...` igual.)
3. Presiona `Ctrl+X` para arrancar.
4. Inicia sesión, NO abras nada que use la GPU, abre una terminal y corre:
   `bash ~/Development/gpu-unlock-hp/gpu-unlock-ada/build/matched-bench/followup-2026-11-04/diag_boot.sh wiped`
5. Espera a que diga `LISTO`.

## Reinicio 2 — "stock" (base limpia de hoy, mismas condiciones)
1. Reinicia. En GRUB presiona `e`.
2. En la línea `linux`, BORRA `gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1`.
   (Deja `nvidia.NVreg_DynamicPowerManagement=0x00` igual.)
3. `Ctrl+X`, inicia sesión y corre:
   `bash ~/Development/gpu-unlock-hp/gpu-unlock-ada/build/matched-bench/followup-2026-11-04/diag_boot.sh stock`

## Al terminar
Reinicia normal (sin tocar GRUB): vuelve el unlock con D3cold bloqueado, como hoy.
Laptop conectada a la corriente durante todo.
