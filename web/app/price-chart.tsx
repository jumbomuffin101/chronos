"use client";
import { useEffect, useRef } from "react";
import type { UTCTimestamp } from "lightweight-charts";
import type { Trade } from "./terminal-state";

export default function PriceChart({ trades }: { trades: Trade[] }) {
  const host = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!host.current || trades.length === 0) return;
    const el = host.current;
    let disposed = false;
    let cleanup: (() => void) | undefined;
    void import("lightweight-charts").then(
      ({ createChart, LineSeries, HistogramSeries, ColorType }) => {
        if (disposed) return;
        const chart = createChart(el, {
          width: el.clientWidth,
          height: 240,
          layout: {
            background: { type: ColorType.Solid, color: "#111922" },
            textColor: "#80909c",
          },
          grid: {
            vertLines: { color: "#1c2933" },
            horzLines: { color: "#1c2933" },
          },
          timeScale: { timeVisible: true, secondsVisible: true },
          rightPriceScale: { borderColor: "#273640" },
        });
        const line = chart.addSeries(LineSeries, {
          color: "#65cfac",
          lineWidth: 2,
          priceLineVisible: false,
        });
        const volume = chart.addSeries(HistogramSeries, {
          priceScaleId: "",
          color: "#405a63",
          priceLineVisible: false,
          lastValueVisible: false,
        });
        chart
          .priceScale("")
          .applyOptions({ scaleMargins: { top: 0.8, bottom: 0 } });
        const buckets = new Map<number, { price: number; quantity: number }>();
        for (const t of [...trades].reverse()) {
          const p = Number(t.price),
            q = Number(t.quantity),
            sec = Math.floor(t.time / 1000);
          if (!Number.isSafeInteger(p) || !Number.isSafeInteger(q)) continue;
          const old = buckets.get(sec);
          const total = (old?.quantity ?? 0) + q;
          if (!Number.isSafeInteger(total)) continue;
          buckets.set(sec, { price: p, quantity: total });
        }
        const entries = [...buckets.entries()].sort((a, b) => a[0] - b[0]);
        line.setData(
          entries.map(([time, v]) => ({
            time: time as UTCTimestamp,
            value: v.price,
          })),
        );
        volume.setData(
          entries.map(([time, v]) => ({
            time: time as UTCTimestamp,
            value: v.quantity,
          })),
        );
        chart.timeScale().fitContent();
        const observer = new ResizeObserver(() =>
          chart.applyOptions({ width: el.clientWidth }),
        );
        observer.observe(el);
        cleanup = () => {
          observer.disconnect();
          chart.remove();
        };
      },
    );
    return () => {
      disposed = true;
      cleanup?.();
    };
  }, [trades]);
  return (
    <div
      ref={host}
      aria-label="Observed session trade price and volume chart"
    />
  );
}
