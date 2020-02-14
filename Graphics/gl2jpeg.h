// Gmsh - Copyright (C) 1997-2020 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#ifndef GL2JPEG_H
#define GL2JPEG_H

#include <stdio.h>
#include "PixelBuffer.h"

void create_jpeg(FILE *outfile, PixelBuffer *buffer, int quality,
                 int smoothing);

#endif
