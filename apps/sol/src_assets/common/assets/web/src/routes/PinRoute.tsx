/**
 * @file PIN pairing route (legacy `pin.html`).
 *
 * Polls pending pairing requests every 2 seconds via TanStack Query.
 */

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Hash, Monitor, UserRoundSearch, X } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { cancelPairing, savePin } from '../api/endpoints'
import type { Pairing } from '../api/schemas'
import { SelectField, TextField } from '../components/fields'
import { Alert, Button, Card, PageHeader } from '../components/ui'
import { pairingsQuery, queryKeys } from '../queries'
import { Page } from './shell'

/** Localized status feedback for the page. */
interface StatusMessage {
  variant: 'success' | 'danger'
  message: string
}

/**
 * @brief PIN pairing page component.
 * @returns The page element.
 */
export default function PinRoute() {
  return (
    <Page>
      <PinContent />
    </Page>
  )
}

/**
 * @brief Pairing form once the shell is loaded.
 * @returns The page content.
 */
function PinContent() {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  const [selectedPairingId, setSelectedPairingId] = useState('')
  const [pin, setPin] = useState('')
  const [deviceName, setDeviceName] = useState('')
  const [status, setStatus] = useState<StatusMessage | null>(null)

  const { data: pendingPairings } = useQuery(pairingsQuery())
  const pairings: Pairing[] = pendingPairings ?? []
  const selectedPairing = pairings.find((pairing) => pairing.id === selectedPairingId)

  const invalidate = () => {
    void queryClient.invalidateQueries({ queryKey: queryKeys.pairings })
  }

  const submitMutation = useMutation({
    mutationFn: () => savePin(selectedPairingId, pin, deviceName),
    onSuccess: (body) => {
      if (body.status === true) {
        setStatus({ variant: 'success', message: t('pin.pair_success') })
        setPin('')
        setDeviceName('')
      } else {
        setStatus({ variant: 'danger', message: t('pin.pair_failure') })
      }
      invalidate()
    },
  })

  const cancelMutation = useMutation({
    mutationFn: () => cancelPairing(selectedPairingId),
    onSuccess: (body) => {
      setStatus(
        body.status === true
          ? { variant: 'success', message: t('pin.cancel_success') }
          : { variant: 'danger', message: t('pin.cancel_failure') },
      )
      invalidate()
    },
  })

  const pairingsOptions = [
    {
      value: '',
      label: pairings.length > 0 ? t('pin.select_pairing') : t('pin.no_pending_pairings'),
    },
    ...pairings.map((pairing) => ({
      value: pairing.id,
      label: `${pairing.name || t('pin.unknown_device')} — ${pairing.address || t('pin.unknown_address')}`,
    })),
  ]

  return (
    <>
      <PageHeader title={t('pin.pin_pairing')} />
      <form
        onSubmit={(event) => {
          event.preventDefault()
          if (!selectedPairingId) {
            setStatus({ variant: 'danger', message: t('pin.select_pairing_required') })
            return
          }
          submitMutation.mutate()
        }}
      >
        <div className="centeredForm">
          <Card>
            <div className="inputWithIcon">
              <UserRoundSearch size={18} aria-hidden />
              <SelectField
                id="pairing-input"
                label={t('pin.select_pairing')}
                value={selectedPairingId}
                onChange={setSelectedPairingId}
                options={pairingsOptions}
              />
              <button
                type="button"
                className="inlineDanger"
                disabled={!selectedPairingId}
                title={t('pin.cancel_pairing')}
                aria-label={t('pin.cancel_pairing')}
                onClick={() => cancelMutation.mutate()}
              >
                <X size={18} aria-hidden />
              </button>
            </div>

            {selectedPairing && (
              <div className="pairingDetails">
                <div>
                  <strong>{t('pin.requested_platform')}:</strong>{' '}
                  {selectedPairing.platform || t('pin.unknown_platform')}
                </div>
                <div>
                  <strong>{t('pin.requested_permissions')}:</strong>{' '}
                  {selectedPairing.requested_scopes.length > 0
                    ? selectedPairing.requested_scopes.join(', ')
                    : selectedPairing.explicit_policy
                      ? t('pin.no_permissions')
                      : t('pin.legacy_permissions')}
                </div>
                {selectedPairing.requested_inputs.length > 0 && (
                  <div>
                    <strong>{t('pin.requested_input')}:</strong>{' '}
                    {selectedPairing.requested_inputs.join(', ')}
                  </div>
                )}
              </div>
            )}

            <div className="inputWithIcon">
              <Hash size={18} aria-hidden />
              <TextField
                id="pin-input"
                label={t('navbar.pin')}
                value={pin}
                onChange={setPin}
                required
              />
            </div>

            <div className="inputWithIcon">
              <Monitor size={18} aria-hidden />
              <TextField
                id="name-input"
                label={t('pin.device_name')}
                value={deviceName}
                onChange={setDeviceName}
                required
              />
            </div>

            <Button type="submit">{t('pin.send')}</Button>
          </Card>
        </div>
        <Alert variant="warning" title={t('_common.warning')}>
          {t('pin.warning_msg')}
        </Alert>
        {status && <Alert variant={status.variant}>{status.message}</Alert>}
      </form>
    </>
  )
}
