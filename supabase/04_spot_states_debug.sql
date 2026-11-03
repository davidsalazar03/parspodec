-- ParSpoDec — dados técnicos (debug) em cada mudança de estado
-- Corre isto no Supabase: SQL Editor → New query → colar tudo → Run
-- IMPORTANTE: correr ANTES de gravar o firmware novo na ESP32.
--
-- Colunas novas (ficam vazias nas linhas antigas):
--   prev_dcode   d-code antes da mudança (vazio = 1.º estado depois de um arranque/reset)
--   new_dcode    d-code depois da mudança
--                  -1 = < 120 cm (perto demais) | 0 = L1 ocupado
--                   1 = L1 livre, L2 ocupado    | 2 = ambos livres
--   reset_reason motivo do arranque, valor de esp_reset_reason():
--                  8 = acordou do deep sleep (normal) | 1 = falta de energia
--                  9 = brownout (tensão baixa)        | 4–7 = crash/watchdog
--                  3 = reset por software             | 2 = botão EN
--   cycle        nº do ciclo (acordar a cada 20 s) em que a mudança foi confirmada;
--                volta a 0 em cada reset
--   send_fails   tentativas de envio falhadas antes desta ter chegado
--   age_s        segundos entre a confirmação da mudança e a gravação aqui
--                (hora real da mudança ≈ created_at − age_s)
--   boot_id      nº aleatório gerado em cada arranque/reset — se muda, a ESP32 reiniciou
--   seq          nº do evento dentro desse arranque

alter table spot_states
  add column prev_dcode   smallint check (prev_dcode between -1 and 2),
  add column new_dcode    smallint check (new_dcode  between -1 and 2),
  add column reset_reason smallint,
  add column cycle        integer,
  add column send_fails   integer,
  add column age_s        integer,
  add column boot_id      bigint,
  add column seq          integer;

-- Impede linhas repetidas: se a ESP32 reenviar um evento que já tinha chegado
-- (a resposta perdeu-se), o servidor recusa com 409 e o firmware dá-o por enviado.
alter table spot_states
  add constraint spot_states_event_unique unique (boot_id, seq);
