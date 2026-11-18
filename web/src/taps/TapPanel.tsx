// SPDX-License-Identifier: GPL-2.0-only
// The panel of whichever tap a view follows (taps/index.ts): one per kind.
import type { TapFrame, TapStatus, TapWatch } from './index';
import { VideoPanel } from './VideoPanel';

export type TapPanelProps = {
  watch: TapWatch;
  status: TapStatus | null;
  frames: (sink: ((f: TapFrame) => void) | null) => void;
  onClose: () => void;
};

export function TapPanel(p: TapPanelProps) {
  switch (p.watch.kind) {
    case 'video':
      return <VideoPanel {...p} />;
  }
}
