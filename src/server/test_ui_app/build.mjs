import { copyFile, mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { build } from "esbuild";

const rootDir = path.dirname(fileURLToPath(import.meta.url));
const outdir = process.env.ZKS_TEST_UI_OUTDIR
  ? path.resolve(process.env.ZKS_TEST_UI_OUTDIR)
  : path.join(rootDir, "dist");

await mkdir(outdir, { recursive: true });

await build({
  entryPoints: [path.join(rootDir, "src", "main.ts")],
  outdir,
  entryNames: "index",
  bundle: true,
  format: "iife",
  target: ["es2022"],
  minify: true,
  sourcemap: false,
  logLevel: "info"
});

await copyFile(path.join(rootDir, "src", "index.html"), path.join(outdir, "index.html"));
