// SPDX-License-Identifier: GPL-2.0-only
// The panel of whichever tap a view follows (taps/index.ts): one per kind.
import type { TapPanelProps } from './index';
import { VideoPanel } from './VideoPanel';

export function TapPanel(p: TapPanelProps) {
  switch (p.watch.kind) {
    case 'video':
      return <VideoPanel {...p} />;
  }
}
