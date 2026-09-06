/**
 * @file Clients route (new page): paired device management backed by
 * `/api/clients/*`. The backend already served `/clients`, but the legacy
 * frontend never shipped a page for it.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  createColumnHelper,
  flexRender,
  getCoreRowModel,
  useReactTable,
} from '@tanstack/react-table'
import { Trash2 } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { unpairAll, unpairClient, updateClient } from '../api/endpoints'
import type { Client } from '../api/schemas'
import { ConfirmDialog } from '../components/Modal'
import { Alert, Badge, Button, Card, Spinner } from '../components/ui'
import { clientsQuery, queryKeys } from '../queries'
import styles from './ClientsRoute.module.css'
import { Page } from './shell'

/**
 * @brief Clients page component.
 * @returns The page element.
 */
export default function ClientsRoute() {
  return (
    <Page>
      <ClientsContent />
    </Page>
  )
}

const columnHelper = createColumnHelper<Client>()

/**
 * @brief Clients table body once the shell is loaded.
 * @returns The page content.
 */
function ClientsContent() {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  const { data: clients, isPending } = useQuery(clientsQuery())
  const [unpairTarget, setUnpairTarget] = useState<Client | null>(null)
  const [unpairAllStatus, setUnpairAllStatus] = useState<boolean | null>(null)
  const [message, setMessage] = useState<string | null>(null)

  const invalidate = () => {
    void queryClient.invalidateQueries({ queryKey: queryKeys.clients })
  }

  const unpairMutation = useMutation({
    mutationFn: (uuid: string) => unpairClient(uuid),
    onSuccess: () => {
      setUnpairTarget(null)
      setMessage(t('troubleshooting.unpair_single_success'))
      invalidate()
    },
  })

  const unpairAllMutation = useMutation({
    mutationFn: unpairAll,
    onSuccess: (body) => {
      setUnpairAllStatus(body.status === true)
      window.setTimeout(() => setUnpairAllStatus(null), 5000)
      invalidate()
    },
  })

  const toggleMutation = useMutation({
    mutationFn: ({ uuid, enabled }: { uuid: string; enabled: boolean }) =>
      updateClient(uuid, enabled),
    onSuccess: invalidate,
  })

  const columns = useMemo(
    () => [
      columnHelper.accessor('name', {
        header: t('_common.username'),
        cell: (info) =>
          info.getValue() !== '' ? (
            info.getValue()
          ) : (
            <em>{t('troubleshooting.unpair_single_unknown')}</em>
          ),
      }),
      columnHelper.accessor('uuid', {
        header: 'UUID',
        cell: (info) => <code className="mono">{info.getValue()}</code>,
      }),
      columnHelper.accessor('enabled', {
        header: t('_common.enabled'),
        cell: (info) => (
          <label className={styles.switchLabel}>
            <input
              type="checkbox"
              role="switch"
              checked={info.getValue()}
              aria-checked={info.getValue()}
              onChange={(event) =>
                toggleMutation.mutate({
                  uuid: info.row.original.uuid,
                  enabled: event.target.checked,
                })
              }
            />
          </label>
        ),
      }),
      columnHelper.display({
        id: 'actions',
        header: '',
        cell: (info) => (
          <Button small variant="danger" onClick={() => setUnpairTarget(info.row.original)}>
            <Trash2 size={16} aria-hidden />
            {t('troubleshooting.unpair_title')}
          </Button>
        ),
      }),
    ],
    [t, toggleMutation],
  )

  const table = useReactTable({
    data: clients ?? [],
    columns,
    getCoreRowModel: getCoreRowModel(),
  })

  if (isPending) {
    return <Spinner />
  }

  return (
    <>
      <div className="appsHeader">
        <h1>{t('clients.title')}</h1>
        <p>{t('troubleshooting.unpair_desc')}</p>
      </div>

      {message && <Alert variant="success">{message}</Alert>}
      {unpairAllStatus === true && (
        <Alert variant="success">{t('troubleshooting.unpair_all_success')}</Alert>
      )}
      {unpairAllStatus === false && (
        <Alert variant="danger">{t('troubleshooting.unpair_all_error')}</Alert>
      )}

      <Card>
        <div className={styles.toolbar}>
          <Badge variant="neutral">{clients?.length ?? 0}</Badge>
          <Button
            variant="danger"
            disabled={unpairAllMutation.isPending}
            onClick={() => unpairAllMutation.mutate()}
          >
            {t('troubleshooting.unpair_all')}
          </Button>
        </div>

        {(clients?.length ?? 0) === 0 ? (
          <p className="mutedCenter">
            <em>{t('troubleshooting.unpair_single_no_devices')}</em>
          </p>
        ) : (
          // biome-ignore lint/a11y/noNoninteractiveTabindex: Keyboard users must focus the overflow region to scroll the clients table horizontally.
          <section className="tableScroll" aria-label={t('clients.title')} tabIndex={0}>
            <table className="table">
              <thead>
                {table.getHeaderGroups().map((headerGroup) => (
                  <tr key={headerGroup.id}>
                    {headerGroup.headers.map((header) => (
                      <th key={header.id}>
                        {header.isPlaceholder
                          ? null
                          : flexRender(header.column.columnDef.header, header.getContext())}
                      </th>
                    ))}
                  </tr>
                ))}
              </thead>
              <tbody>
                {table.getRowModel().rows.map((row) => (
                  <tr key={row.id}>
                    {row.getVisibleCells().map((cell) => (
                      <td key={cell.id}>
                        {flexRender(cell.column.columnDef.cell, cell.getContext())}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </section>
        )}
      </Card>

      <ConfirmDialog
        open={unpairTarget !== null}
        title={t('troubleshooting.unpair_title')}
        message={t('apps.delete_confirm', { name: unpairTarget?.name ?? '' })}
        confirmLabel={t('troubleshooting.unpair_title')}
        onConfirm={() => {
          if (unpairTarget) {
            unpairMutation.mutate(unpairTarget.uuid)
          }
        }}
        onCancel={() => setUnpairTarget(null)}
      />
    </>
  )
}
