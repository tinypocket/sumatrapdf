import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { detectVisualStudio, runLogged } from "./util";

const solution = String.raw`vs2022\SumatraPDF.sln`;
const packageDir = join("out", "packages");

const builds = [
  { platform: "x64", outDir: join("out", "rel64"), suffix: "64" },
  { platform: "ARM64", outDir: join("out", "arm64"), suffix: "arm64" },
];

function sha256(path: string): string {
  return createHash("sha256").update(readFileSync(path)).digest("hex").toUpperCase();
}

async function main() {
  await runLogged(process.execPath, ["cmd/premake.ts"]);
  const { msbuildPath } = detectVisualStudio();
  mkdirSync(packageDir, { recursive: true });

  const hashes: string[] = [];
  for (const build of builds) {
    const properties = `/p:Configuration=Release;Platform=${build.platform}`;
    await runLogged(msbuildPath, [solution, "/t:SumatraPDF;SumatraPDF-static", properties, "/m"]);

    const artifacts = [
      {
        source: join(build.outDir, "SumatraPDF.exe"),
        name: `SumatraPDF+-private-${build.suffix}-install.exe`,
      },
      {
        source: join(build.outDir, "SumatraPDF-static.exe"),
        name: `SumatraPDF+-private-${build.suffix}-portable.exe`,
      },
    ];
    for (const artifact of artifacts) {
      const destination = join(packageDir, artifact.name);
      copyFileSync(artifact.source, destination);
      hashes.push(`${sha256(destination)} *${artifact.name}`);
    }
  }

  const sumsPath = join(packageDir, "SHA256SUMS.txt");
  writeFileSync(sumsPath, `${hashes.join("\n")}\n`);
  console.log(`SumatraPDF+ packages and SHA-256 hashes are in ${packageDir}`);
}

await main();
