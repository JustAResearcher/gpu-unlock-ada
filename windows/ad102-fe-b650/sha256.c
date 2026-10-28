#include "sha256.h"

/* FIPS 180-4, sections 4.2.2 and 6.2. All arithmetic is unsigned. */
static const UINT32 RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static UINT32 RotateRight(UINT32 Value, unsigned Bits)
{
    return (Value >> Bits) | (Value << (32u - Bits));
}

static VOID Transform(UINT32 State[8], const UINT8 Block[64])
{
    UINT32 Words[64];
    UINT32 A = State[0];
    UINT32 B = State[1];
    UINT32 C = State[2];
    UINT32 D = State[3];
    UINT32 E = State[4];
    UINT32 F = State[5];
    UINT32 G = State[6];
    UINT32 H = State[7];
    unsigned Index;

    /* Byte loads avoid alignment and host-endianness assumptions. */
    for (Index = 0; Index < 16; ++Index) {
        Words[Index] = ((UINT32)Block[4 * Index] << 24) |
                       ((UINT32)Block[4 * Index + 1] << 16) |
                       ((UINT32)Block[4 * Index + 2] << 8) |
                       (UINT32)Block[4 * Index + 3];
    }
    for (Index = 16; Index < 64; ++Index) {
        UINT32 X = Words[Index - 15];
        UINT32 Y = Words[Index - 2];
        UINT32 Small0 = RotateRight(X, 7) ^ RotateRight(X, 18) ^ (X >> 3);
        UINT32 Small1 = RotateRight(Y, 17) ^ RotateRight(Y, 19) ^ (Y >> 10);
        Words[Index] = Words[Index - 16] + Small0 + Words[Index - 7] + Small1;
    }
    for (Index = 0; Index < 64; ++Index) {
        UINT32 Big0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
        UINT32 Big1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
        UINT32 Choice = (E & F) ^ (~E & G);
        UINT32 Majority = (A & B) ^ (A & C) ^ (B & C);
        UINT32 Temp1 = H + Big1 + Choice + RoundConstants[Index] + Words[Index];
        UINT32 Temp2 = Big0 + Majority;

        H = G;
        G = F;
        F = E;
        E = D + Temp1;
        D = C;
        C = B;
        B = A;
        A = Temp1 + Temp2;
    }
    State[0] += A;
    State[1] += B;
    State[2] += C;
    State[3] += D;
    State[4] += E;
    State[5] += F;
    State[6] += G;
    State[7] += H;
}

VOID GuSha256(const VOID *Data, UINTN Size, UINT8 Digest[32])
{
    UINT32 State[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    const UINT8 *Bytes = (const UINT8 *)Data;
    UINT64 BitLength = (UINT64)Size * 8u;
    UINT8 Tail[64] = {0};
    UINTN Index;

    while (Size >= 64) {
        Transform(State, Bytes);
        Bytes += 64;
        Size -= 64;
    }
    for (Index = 0; Index < Size; ++Index) {
        Tail[Index] = Bytes[Index];
    }
    Tail[Size] = 0x80;
    if (Size >= 56) {
        Transform(State, Tail);
        for (Index = 0; Index < sizeof(Tail); ++Index) {
            Tail[Index] = 0;
        }
    }
    for (Index = 0; Index < 8; ++Index) {
        Tail[63 - Index] = (UINT8)(BitLength >> (8 * Index));
    }
    Transform(State, Tail);
    for (Index = 0; Index < 8; ++Index) {
        Digest[4 * Index] = (UINT8)(State[Index] >> 24);
        Digest[4 * Index + 1] = (UINT8)(State[Index] >> 16);
        Digest[4 * Index + 2] = (UINT8)(State[Index] >> 8);
        Digest[4 * Index + 3] = (UINT8)State[Index];
    }
}

static int HexValue(char Character)
{
    if (Character >= '0' && Character <= '9') {
        return Character - '0';
    }
    if (Character >= 'a' && Character <= 'f') {
        return Character - 'a' + 10;
    }
    if (Character >= 'A' && Character <= 'F') {
        return Character - 'A' + 10;
    }
    return -1;
}

BOOLEAN GuHashMatches(const VOID *Data, UINTN Size, const char *Hex64)
{
    UINT8 Expected[32];
    UINT8 Actual[32];
    UINT8 Difference = 0;
    unsigned Index;

    if (Hex64 == NULL || (Data == NULL && Size != 0) ||
        (UINT64)Size > (~(UINT64)0 >> 3)) {
        return FALSE;
    }
    for (Index = 0; Index < 32; ++Index) {
        int High = HexValue(Hex64[2 * Index]);
        int Low;

        /* Stop before the next read when a short string reaches its NUL. */
        if (High < 0) {
            return FALSE;
        }
        Low = HexValue(Hex64[2 * Index + 1]);
        if (Low < 0) {
            return FALSE;
        }
        Expected[Index] = (UINT8)((High << 4) | Low);
    }
    if (Hex64[64] != '\0') {
        return FALSE;
    }
    GuSha256(Data, Size, Actual);
    for (Index = 0; Index < 32; ++Index) {
        Difference |= Actual[Index] ^ Expected[Index];
    }
    return Difference == 0 ? TRUE : FALSE;
}
