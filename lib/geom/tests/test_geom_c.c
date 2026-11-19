#include "pr/geom.h"

int prTestGeomCApi(void);

// The public header compiled as C. Values are plain structures.
int prTestGeomCApi(void) {
    PrRect rect = {0, 0, 4, 2};
    PrAffine2 scale = prAffine2Scale((PrVec2){2, 2});
    PrRect mapped = prAffine2MapRect(scale, rect);
    PrCornerRect shape = prCornerRectRound(rect, 1);
    PrAffine2 inverse = prAffine2Identity();
    int result = mapped.right != 8 || mapped.bottom != 4;
    result |= prCornerRectFit(shape).corners[PrRectCorner_BottomLeft].radii.x != 1;
    result |= !prAffine2Invert(scale, &inverse) || inverse.a != 0.5f;
    PrAffine3 same = prAffine3Identity();
    PrMatrix4 view = prMatrix4LookAt((PrVec3){0, 0, 5}, (PrVec3){0, 0, 0});
    PrAffine3 toView = prAffine3FromMatrix4(&view);
    PrBox3 box = {{-1, -1, -1}, {1, 1, 1}};
    PrSphere sphere = {{0, 0, 0}, 2};
    result |= prAffine3MapPoint(&same, box.max).z != 1 || prAffine3MapPoint(&toView, sphere.center).z != -5;
    result |= prAffine3MaxScale(&same) != sphere.radius - 1;
    return result;
}
