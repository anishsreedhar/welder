"""OpenPose-style pose images from DWPose (adapted from easy_dwpose's DWposeDetector)."""

import cv2
import numpy as np

from .draw_openpose import draw_pose
from .utils import resize_image
from .wholebody import Wholebody


class DWposeDetector:
    def __init__(self, model_det, model_pose, device="cpu"):
        self.pose_estimation = Wholebody(device=device, model_det=model_det, model_pose=model_pose)

    @staticmethod
    def _format_pose(candidates, scores, width, height):
        num_candidates, _, locs = candidates.shape
        candidates[..., 0] /= float(width)
        candidates[..., 1] /= float(height)
        bodies = candidates[:, :18].copy().reshape(num_candidates * 18, locs)
        body_scores = scores[:, :18]
        for i in range(len(body_scores)):
            for j in range(len(body_scores[i])):
                body_scores[i][j] = int(18 * i + j) if body_scores[i][j] > 0.3 else -1
        faces = candidates[:, 24:92]
        faces_scores = scores[:, 24:92]
        hands = np.vstack([candidates[:, 92:113], candidates[:, 113:]])
        hands_scores = np.vstack([scores[:, 92:113], scores[:, 113:]])
        return dict(bodies=bodies, body_scores=body_scores, hands=hands, hands_scores=hands_scores,
                    faces=faces, faces_scores=faces_scores)

    def __call__(self, image_u8, detect_resolution=512, **kw):
        """image_u8: H x W x 3 RGB uint8. Returns the pose drawing, H x W x 3 uint8 (black background)."""
        h0, w0 = image_u8.shape[:2]
        image = resize_image(np.ascontiguousarray(image_u8), target_resolution=detect_resolution)
        height, width = image.shape[:2]
        candidates, scores = self.pose_estimation(image)
        pose = self._format_pose(candidates, scores, width, height)
        out = draw_pose(pose, height=height, width=width, **kw)
        return cv2.resize(out, (w0, h0), interpolation=cv2.INTER_LANCZOS4)
