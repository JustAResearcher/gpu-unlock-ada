# 외부 코드 고지

## 2026-10-23 내부 부팅 확장

`internal-boot/`는 기존 GPL-2.0-only 포트의 별도 실험 확장입니다. 새 콘솔/드라이버
인계, 제한된 PCI 복원·비교, 자동 Arm, 설치/복구 코드와 대응 검사를 추가했습니다.
기존 root `unlock/`·릴리스의 대응 소스는 보존했습니다. 같은 EDK2 헤더는 root `deps/`를
사용하며 라이선스 고지를 유지합니다. 바이너리 NVIDIA 펌웨어·PROM·BIOS 이미지는
포함하지 않았습니다. 파티션에 종속되는 EFI는 공개 소스로 사용자가 로컬 빌드합니다.

## Windows GPU UNLOCK — RTX 3090 포트

2026-10-19/20 수정. 직접 기반은 [rough2515-hub/rtx3080-unlock-public](https://github.com/rough2515-hub/rtx3080-unlock-public/tree/a08254d269af30ace9e57237abd3d8e6c261e8f5)입니다. RTX3090 24GiB / AMD iGPU 대상 검사·메모리 배치·entryLatchedSs1Full 프로필과 대응 테스트를 추가했습니다. 상세 변경 파일과 내용은 [CHANGES.md](CHANGES.md)를 참고하세요. 아래는 상위 코드의 보존 고지입니다. 하위의 MSI3080 관련 날짜·설명은 상위 코드의 이력이며 현재 지원 범위는 README와 docs/COMPATIBILITY.ko.md가 설명합니다.

별도 펌웨어 바이너리의 재배포 권한을 확인했다는 의미가 아닙니다. 이 저장소와 공개 릴리스는 펌웨어 5개를 포함하지 않습니다.

## GPU unlock 코어 및 UEFI 포트 — GPL-2.0-only

- 원본: [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock/tree/0ebdb1c6308b6a2072be5bf04513472942541956)
- 고정 커밋: `0ebdb1c6308b6a2072be5bf04513472942541956`
- 대상: `unlock/vendor/`의 `unlock_610`, `prep_610`, `layout_610`, `vbios_610`, `gfw_eval`, `diagnostic.h`.
- 라이선스 원문: [LICENSE](LICENSE).

2026-10-18의 로컬 변경: MSI RTX 3080 대상 선택, UEFI의 checked I/O 및 실행·정리 상태 연결, FWSEC 진단. `tests/reference/unlock_610_v06.c`는 비교 검사에 사용하는 로컬 v0.6 스냅샷입니다. UEFI 포트와 host/mock 테스트도 GPL-2.0-only로 제공합니다. v0.10에서는 PCI I/O Attributes를 통해 Bus Master를 관리하고 프로토콜 상태와 실제 PCI Command를 함께 검사합니다.

2026-10-19의 공개용 도구·문서 정리: 로컬 USB 패키지 생성, 공개용 테스트 실행기, 상대 경로 기반 빌드 메타데이터. GPU 실행 조건과 transaction 순서는 유지했습니다.

## EDK2 헤더 — BSD-2-Clause-Patent

- 원본: [TianoCore EDK2](https://github.com/tianocore/edk2/tree/c1415771ac592ad8509351c46cd3cda4f38d7d52)
- 고정 커밋: `c1415771ac592ad8509351c46cd3cda4f38d7d52`
- 대상: `deps/edk2/MdePkg/Include/`.
- 원문과 저작권 고지: [EDK2 라이선스](LICENSES/EDK2-BSD-2-Clause-Patent.txt) 및 각 파일의 헤더.

앱은 헤더 선언을 사용하며 EDK2 라이브러리 바이너리를 링크하지 않습니다.

## 상위 프로젝트의 관련 고지

상위 프로젝트에서 보존한 NVIDIA 소스 및 EFI 참조 고지를 함께 제공합니다.

- [NVIDIA MIT 고지](LICENSES/NVIDIA-MIT.txt)
- [CMP40HX MIT 고지](LICENSES/CMP40HX-MIT.txt)
- [NVIDIA Driver License Agreement](LICENSES/NVIDIA-Driver.txt)

소스와 USB 패키지를 복사할 때 해당 라이선스와 저작권 고지를 함께 유지하세요.
