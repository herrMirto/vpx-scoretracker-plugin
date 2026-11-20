# Viewer interface

The single TypeScript/Vite interface is embedded in the native Viewer. It displays
local game history and read-only machine high scores. Native folder selection,
settings, confirmed deletion and logs are provided by the TypeScript backend
running in Electron. The renderer has no Node.js or filesystem access.
There is no uploader, signing interface, plugin installer or automatic updater.

```sh
npm ci
npm run check
npm test
npm run build
npm start
```

`npm run dev` starts Vite and Electron using temporary settings.
See [Viewer development and packaging](../viewer/README.md).
