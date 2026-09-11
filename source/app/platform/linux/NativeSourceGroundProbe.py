#!/usr/bin/env python3
"""Extract local reversed function bodies, compile pure query, run independent oracle.

Run in mad-sa:dev: python3 /workspace/gta-reversed/source/app/platform/linux/NativeSourceGroundProbe.py --game /game
Only generated executables/objects/logs go under artifacts/graphics. Asset bytes
stay read-only in memory. Oracle bodies are compiled from stdin, never copied to
product source. No CMake or existing product file changes are needed.
"""
import argparse
import hashlib
import pathlib
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game")
    args = parser.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    source = here.parents[2]
    root = source.parent.parent
    out = root / "artifacts/graphics"
    assert out.is_dir()
    collision = source / "game_sa/Collision/Collision.cpp"
    bodies = []
    provenance = []

    def extract(path, signature, prefix="", replacement=None):
        text = path.read_text()
        body = function(text, signature)
        provenance.append(f"{path.relative_to(root)}:{text[:text.index(signature)].count(chr(10))+1} "
                          f"sha256={hashlib.sha256(body.encode()).hexdigest()} {signature}")
        if replacement:
            body = body.replace(signature, replacement, 1)
        bodies.append(prefix + body)

    for signature in ("float DotProduct(const CVector& v1, const CVector& v2)",
                      "auto CVector::Dot(const CVector& o) const -> float",
                      "float CVector::NormaliseAndMag()", "CVector CVector::Cross(const CVector& o) const"):
        extract(source / "game_sa/Core/Vector.cpp", signature)
    for signature, replacement in (
        ("CMatrix Inverted() const", "CMatrix CMatrix::Inverted() const"),
        ("CVector TransformPoint(CVector pt) const", "CVector CMatrix::TransformPoint(CVector pt) const"),
        ("CVector TransformVector(CVector v) const", "CVector CMatrix::TransformVector(CVector v) const"),
    ):
        extract(source / "game_sa/Core/Matrix.h", signature, replacement=replacement)
    for signature in ("void CMatrix::SetRotate(const CQuaternion& quat)", "void CMatrix::SetRotateZOnly(float angle)"):
        extract(source / "game_sa/Core/Matrix.cpp", signature)
    extract(source / "game_sa/Collision/BoundingBox.cpp", "bool CBoundingBox::IsPointWithin(const CVector& point) const")
    extract(source / "game_sa/Collision/ColTrianglePlane.cpp", "CColTrianglePlane::CColTrianglePlane(const CVector& vA, const CVector& vB, const CVector& vC)")
    extract(source / "game_sa/common.h", "T lerp(const T& from, const T& to, float t)", "template<class T>\n")
    for signature in (
        "NOTSA_FORCEINLINE bool ProcessLineSphere_Internal(",
        "bool CCollision::ProcessLineSphere(CColLine const& line",
        "bool CCollision::TestLineBox_DW(CColLine const& line",
        "bool CCollision::ProcessLineBox(CColLine const& line",
        "bool NOTSA_FORCEINLINE ProcessLineTriangle_Internal(",
        "bool CCollision::ProcessLineTriangle(const CColLine& line",
    ):
        extract(collision, signature, "template<bool TestOnly>\n" if "ProcessLineTriangle_Internal" in signature else "")
    # Prove policy/byte-layout premises against independent source, rather than
    # letting a hand-written fixture invent a generic collision flag meaning.
    vertical = function(collision.read_text(), "bool CCollision::ProcessVerticalLine(\n")
    assert "(!doSeeThroughCheck || g_surfaceInfos.IsSeeThrough(surf))" in vertical
    assert "//&& (!doShootThroughCheck" in vertical
    assert "lnos.m_vecEnd.x = lnos.m_vecStart.x" in vertical
    assert "lnos.m_vecEnd.y = lnos.m_vecStart.y" in vertical
    assert vertical.index("Process(cd->GetSpheres()") < vertical.index("Process(cd->GetBoxes()") < vertical.index("CalculateTrianglePlanes(cd)")
    for relative in ("game_sa/Collision/ColSurface.h", "game_sa/Collision/ColHelpers.h", "game_sa/FileLoader.cpp",
                     "extensions/FixedFloat.hpp", "extensions/FixedVector.hpp", "game_sa/CompressedVector.h"):
        content = (source / relative).read_bytes()
        provenance.append(f"gta-reversed/source/{relative} sha256={hashlib.sha256(content).hexdigest()}")
    provenance.append("Collision::ProcessVerticalLine body sha256=" + hashlib.sha256(vertical.encode()).hexdigest())
    probe = (here / "NativeSourceGroundProbe.cpp").read_text()
    assert probe.count("// SOURCE_ORACLE_INSERT") == 1
    probe = probe.replace("// SOURCE_ORACLE_INSERT", "\n\n".join(bodies))
    surface = function((source / "game_sa/Collision/ColSurface.h").read_text(), "struct CColSurface") + ";"
    assert probe.count("// SOURCE_SURFACE_INSERT") == 1
    probe = probe.replace("// SOURCE_SURFACE_INSERT", surface)
    flags = ["g++", "-std=c++20", "-O2", "-g", "-ffp-contract=off", "-fno-fast-math", "-ffunction-sections", "-fdata-sections",
             "-I" + str(source), "-I" + str(here), "-Wall", "-Wextra"]
    stem = out / "NativeSourceGroundProbe"
    with stem.with_suffix(".build.log").open("w") as log:
        for unit in ("NativeSourceGround", "NativeCollisionAssets", "NativeWorldEntityInfo"):
            subprocess.run(flags + ["-c", str(here / (unit + ".cpp")), "-o", str(out / (unit + ".probe.o"))],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(flags + ["-x", "c++", "-", "-c", "-o", str(stem.with_suffix(".o"))], input=probe,
                       text=True, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(["g++", "-Wl,--gc-sections", str(stem.with_suffix(".o")), str(out / "NativeSourceGround.probe.o"),
                        str(out / "NativeCollisionAssets.probe.o"), str(out / "NativeWorldEntityInfo.probe.o"), "-o", str(stem)], stdout=log, stderr=subprocess.STDOUT, check=True)
    run = subprocess.run([str(stem)] + ([args.game] if args.game else []), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    stem.with_suffix(".log").write_text("\n".join(provenance) + "\n" + run.stdout)
    print(run.stdout, end="")
    run.check_returncode()


if __name__ == "__main__":
    main()
