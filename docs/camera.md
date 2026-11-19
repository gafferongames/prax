# Cameras

`lib/camera` holds what a 3D view sees by, and the controllers that move it. It depends on `lib/geom` alone, so a
renderer, a picker, a tool that frames a selection and a physics debugger share one camera without a GPU. The header
`pr/camera.h` states each call's contract. This file covers what the library is for, how its parts relate, and what it
promises.

## Cameras

A camera (`PrCamera`) is the matrix that maps the world to the camera's space and the matrix that maps that space to
clip space, in lib/geom's conventions ([render.md](render.md#responsibilities)): right-handed, y up, looking along
negative z, with reversed depth and no far plane.

- **A camera is two matrices and nothing else.** It holds no controller, no input and no target. Whatever made it, a
  controller, an asset's own camera (`prModelPlacementCamera`) or an application's code, gives the same type.
- **Constructors are plain functions**: a camera that looks from a point toward another (`prCameraLookAt`), and the
  perspective and orthographic projections that a camera is built from.
- **A camera answers what a view needs of it**: its position and direction in the world, the ray through a point of
  the view, for picking (`prCameraRay`), and its frustum's planes.

## Controllers

A controller is the state of one way of moving a camera, and makes a camera each frame.

- **A controller takes input as values, not events.** The application maps its own events to a controller's input
  (`PrCameraInput`): a drag in pixels, a wheel's steps, a pinch's scale, and keys held as directions. So a controller
  depends on no windowing library, and one controller serves a window, a page and a test that feeds it numbers.
- **A controller is advanced by time** (`prCameraOrbitAdvance`), so its motion's damping is the same at any frame
  rate, and a capture that advances it by fixed steps is reproducible.
- **The orbit controller** (`PrCameraOrbit`) turns about a target by a yaw and a pitch, at a distance, and pans the
  target in the view's plane. Its pitch stops short of the poles, and its distance stays within limits that a fit sets.
- **Fly and first person** follow, each as a controller of its own with the same input.

## Framing

A controller fits what it shows to a sphere (`prCameraOrbitFit`): the distance at which the sphere fills the view's
field, the near distance that keeps the sphere in front of it, and the limits of zoom. A sphere is all that a fit
reads, so the camera and whatever gives the sphere depend on neither: `lib/stage` gives the sphere of what it presents
([stage.md](stage.md)), a model gives its placement's bounds, and a tool gives its selection's.
