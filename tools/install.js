#!/usr/bin/env node
'use strict';
if (process.env.CI === 'true' || process.env.WEBZERO_SKIP_INSTALL === 'true') {
    console.log('webzero: skipping native download');
} else {
    require('./lib/install').install().catch(err => {
        console.warn('webzero: native binary unavailable: '+err.message);
        console.warn('Build from source or use wz serve --js. Build and inspect remain available.');
    });
}
