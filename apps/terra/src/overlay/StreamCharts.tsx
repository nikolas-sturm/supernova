import { defineChart, lineY, ruleY } from '@tanstack/charts'
import { Chart } from '@tanstack/charts/react'
import { scaleLinear } from '@tanstack/charts/scales/linear'
import { useMemo } from 'react'
import type { StreamStatisticsUpdate } from './overlayProtocol'
import styles from './StreamOverlay.module.css'

type ChartRow = StreamStatisticsUpdate['statistics'] & {
  elapsedSeconds: number
  frameIntervalMs: number
}

interface StreamChartsProps {
  samples: readonly StreamStatisticsUpdate[]
  targetFps?: number | undefined
  bitrateCapKbps?: number | undefined
}

const theme = {
  foreground: '#f3f4ef',
  muted: '#7e8089',
  grid: '#25262d',
  background: 'transparent',
  palette: ['#d7ff4f', '#9d7bff', '#46c8ff', '#ff8f70'],
}

function latestValue(value: number | undefined, unit: string, digits = 1) {
  return value === undefined ? '--' : `${value.toFixed(digits)}${unit}`
}

export function StreamCharts({ samples, targetFps = 60, bitrateCapKbps }: StreamChartsProps) {
  const rows = useMemo<ChartRow[]>(
    () =>
      samples.map(({ elapsedMs, statistics }) => ({
        ...statistics,
        elapsedSeconds: elapsedMs / 1000,
        frameIntervalMs: statistics.presentedFps > 0 ? 1000 / statistics.presentedFps : 0,
      })),
    [samples],
  )
  const latest = rows.at(-1)
  const frameBudget = 1000 / targetFps
  const bitrateCap = bitrateCapKbps ? bitrateCapKbps / 1000 : undefined

  const fps = useMemo(
    () =>
      defineChart({
        marks: [
          ruleY([targetFps], { stroke: '#d7ff4f', strokeDasharray: '3 4', strokeOpacity: 0.35 }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'receivedFps',
            stroke: '#46c8ff',
            strokeWidth: 1.5,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'decodedFps',
            stroke: '#9d7bff',
            strokeWidth: 1.5,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'presentedFps',
            stroke: '#d7ff4f',
            strokeWidth: 2,
          }),
        ],
        scales: {
          x: { scale: scaleLinear, axis: { ticks: { count: 3, format: (value) => `${value}s` } } },
          y: { scale: scaleLinear, nice: true, grid: true, axis: { ticks: { count: 3 } } },
        },
        theme,
      }),
    [rows, targetFps],
  )
  const frameTime = useMemo(
    () =>
      defineChart({
        marks: [
          ruleY([frameBudget], {
            stroke: '#d7ff4f',
            strokeDasharray: '3 4',
            strokeOpacity: 0.35,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'frameIntervalMs',
            stroke: '#d7ff4f',
            strokeWidth: 2,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'averageDecodeMs',
            stroke: '#9d7bff',
            strokeWidth: 1.5,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'averageQueueDelayMs',
            stroke: '#ff8f70',
            strokeWidth: 1.5,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'averagePresentMs',
            stroke: '#46c8ff',
            strokeWidth: 1.5,
          }),
        ],
        scales: {
          x: { scale: scaleLinear, axis: { ticks: { count: 3, format: (value) => `${value}s` } } },
          y: { scale: scaleLinear, nice: true, grid: true, axis: { ticks: { count: 3 } } },
        },
        theme,
      }),
    [frameBudget, rows],
  )
  const bandwidth = useMemo(
    () =>
      defineChart({
        marks: [
          ...(bitrateCap
            ? [
                ruleY([bitrateCap], {
                  stroke: '#9d7bff',
                  strokeDasharray: '3 4',
                  strokeOpacity: 0.45,
                }),
              ]
            : []),
          lineY(rows, { x: 'elapsedSeconds', y: 'bitrateMbps', stroke: '#46c8ff', strokeWidth: 2 }),
        ],
        scales: {
          x: { scale: scaleLinear, axis: { ticks: { count: 3, format: (value) => `${value}s` } } },
          y: { scale: scaleLinear, nice: true, grid: true, axis: { ticks: { count: 3 } } },
        },
        theme,
      }),
    [bitrateCap, rows],
  )
  const latency = useMemo(
    () =>
      defineChart({
        marks: [
          lineY(rows, { x: 'elapsedSeconds', y: 'rttMs', stroke: '#d7ff4f', strokeWidth: 2 }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'rttVarianceMs',
            stroke: '#ff8f70',
            strokeWidth: 1.5,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'averageHostLatencyMs',
            stroke: '#9d7bff',
            strokeWidth: 1.5,
          }),
        ],
        scales: {
          x: { scale: scaleLinear, axis: { ticks: { count: 3, format: (value) => `${value}s` } } },
          y: { scale: scaleLinear, nice: true, grid: true, axis: { ticks: { count: 3 } } },
        },
        theme,
      }),
    [rows],
  )
  const loss = useMemo(
    () =>
      defineChart({
        marks: [
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'frameLossPercent',
            stroke: '#ff8f70',
            strokeWidth: 2,
          }),
          lineY(rows, {
            x: 'elapsedSeconds',
            y: 'jitterLossPercent',
            stroke: '#9d7bff',
            strokeWidth: 1.5,
          }),
        ],
        scales: {
          x: { scale: scaleLinear, axis: { ticks: { count: 3, format: (value) => `${value}s` } } },
          y: { scale: scaleLinear, nice: true, grid: true, axis: { ticks: { count: 3 } } },
        },
        theme,
      }),
    [rows],
  )

  const cards = [
    {
      title: 'Frame rate',
      value: latestValue(latest?.presentedFps, ' fps'),
      definition: fps,
      label: 'Received, decoded, and presented frame rate',
    },
    {
      title: 'Frame time',
      value: latestValue(latest?.frameIntervalMs, ' ms'),
      definition: frameTime,
      label: 'Frame interval and pipeline timing',
    },
    {
      title: 'Bandwidth',
      value: latestValue(latest?.bitrateMbps, ' Mbps'),
      definition: bandwidth,
      label: 'Stream bandwidth',
    },
    {
      title: 'Latency',
      value: latestValue(latest?.rttMs, ' ms', 0),
      definition: latency,
      label: 'Round trip and host latency',
    },
    {
      title: 'Network frame loss',
      value: latestValue(latest?.frameLossPercent, '%'),
      definition: loss,
      label: 'Network frame and jitter loss',
    },
  ]

  return (
    <div className={styles.chartStack}>
      {cards.map((card) => (
        <section className={styles.chartCard} key={card.title}>
          <header>
            <span>{card.title}</span>
            <strong>{card.value}</strong>
          </header>
          {rows.length > 0 ? (
            <Chart
              definition={card.definition}
              height={116}
              initialWidth={360}
              ariaLabel={card.label}
            />
          ) : (
            <div className={styles.chartWaiting}>Waiting for first sample</div>
          )}
        </section>
      ))}
    </div>
  )
}
