from __future__ import annotations

import torch
from torch import nn

from .model_contract_constants import INPUT_CHANNELS, OUTPUT_WIDTH

TEXTNESS_CLASSES = ("text_only", "mixed", "non_text")
ROLE_CLASSES = (
    "heading",
    "paragraph",
    "ui_label",
    "caption_metadata",
    "data_code",
    "other",
)
RELATION_CLASSES = ("single", "split_required", "merge_required", "none")
PATCH_MODE_CLASSES = ("rect_safe", "mask_required", "none")

TEXTNESS_SLICE = slice(0, 3)
ROLE_SLICE = slice(3, 9)
RELATION_SLICE = slice(9, 13)
PATCH_MODE_SLICE = slice(13, 16)
ERASE_BOX_SLICE = slice(16, 20)
LAYOUT_BOX_SLICE = slice(20, 24)


class ConvNormAct(nn.Sequential):
    def __init__(
        self,
        in_channels: int,
        out_channels: int,
        kernel: int,
        stride: int = 1,
        groups: int = 1,
    ):
        padding = kernel // 2
        super().__init__(
            nn.Conv2d(
                in_channels,
                out_channels,
                kernel_size=kernel,
                stride=stride,
                padding=padding,
                groups=groups,
                bias=False,
            ),
            nn.BatchNorm2d(out_channels),
            nn.SiLU(inplace=True),
        )


class InvertedBlock(nn.Module):
    def __init__(
        self, in_channels: int, out_channels: int, stride: int, expansion: int = 4
    ):
        super().__init__()
        hidden = in_channels * expansion
        self.residual = stride == 1 and in_channels == out_channels
        self.layers = nn.Sequential(
            ConvNormAct(in_channels, hidden, 1),
            ConvNormAct(hidden, hidden, 3, stride=stride, groups=hidden),
            nn.Conv2d(hidden, out_channels, kernel_size=1, bias=False),
            nn.BatchNorm2d(out_channels),
        )
        self.activation = nn.SiLU(inplace=True)

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        outputs = self.layers(inputs)
        if self.residual:
            outputs = outputs + inputs
        return self.activation(outputs)


class UiRegionNet(nn.Module):
    """Small randomly initialized CNN with a single packed ONNX output."""

    def __init__(self) -> None:
        super().__init__()
        self.features = nn.Sequential(
            ConvNormAct(INPUT_CHANNELS, 24, 3, stride=2),
            InvertedBlock(24, 32, stride=2),
            InvertedBlock(32, 32, stride=1),
            InvertedBlock(32, 48, stride=2),
            InvertedBlock(48, 48, stride=1),
            InvertedBlock(48, 72, stride=2),
            InvertedBlock(72, 96, stride=2),
            InvertedBlock(96, 128, stride=1),
            InvertedBlock(128, 160, stride=1),
            InvertedBlock(160, 192, stride=1),
        )
        self.classification_head = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
            nn.Linear(192, 192),
            nn.SiLU(inplace=True),
            nn.Dropout(p=0.15),
            nn.Linear(192, 16),
        )
        self.geometry_head = nn.Sequential(
            ConvNormAct(192, 64, 1),
            nn.AdaptiveAvgPool2d((3, 8)),
            nn.Flatten(),
            nn.Linear(64 * 3 * 8, 128),
            nn.SiLU(inplace=True),
            nn.Dropout(p=0.10),
            nn.Linear(128, OUTPUT_WIDTH - 16),
        )
        self.reset_parameters()

    def reset_parameters(self) -> None:
        for module in self.modules():
            if isinstance(module, nn.Conv2d):
                nn.init.kaiming_normal_(
                    module.weight, mode="fan_out", nonlinearity="relu"
                )
            elif isinstance(module, nn.BatchNorm2d):
                nn.init.ones_(module.weight)
                nn.init.zeros_(module.bias)
            elif isinstance(module, nn.Linear):
                nn.init.trunc_normal_(module.weight, std=0.02)
                nn.init.zeros_(module.bias)

    def forward(self, regions: torch.Tensor) -> torch.Tensor:
        features = self.features(regions)
        classification = self.classification_head(features)
        geometry = torch.sigmoid(self.geometry_head(features))
        return torch.cat((classification, geometry), dim=1)


def parameter_count(model: nn.Module) -> int:
    return sum(parameter.numel() for parameter in model.parameters())
