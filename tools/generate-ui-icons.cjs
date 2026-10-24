#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Developer dependency: sharp. Run with NODE_PATH pointing to its node_modules.
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const root = path.resolve(__dirname, '../src/apps/res/icons/remix');

async function main() {
  for (const [theme, color] of [['light', '#334155'], ['dark', '#e2e8f0']]) {
    for (const size of [32, 64, 128]) {
      const output = path.join(root, theme, String(size));
      fs.mkdirSync(output, { recursive: true });
      for (const file of fs.readdirSync(path.join(root, 'svg')).sort()) {
        if (!file.endsWith('.svg')) continue;
        const svg = fs.readFileSync(path.join(root, 'svg', file), 'utf8').replace(/currentColor/g, color);
        await sharp(Buffer.from(svg)).resize(size, size).png().toFile(path.join(output, file.replace('.svg', '.png')));
      }
    }
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
