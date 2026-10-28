-- ParSpoDec — pedidos de distância (debug)
-- Corre isto no Supabase: SQL Editor → New query → colar tudo → Run
--
-- Como funciona:
--   1. No site carregas em "Pedir distância" → é criada uma linha aqui, vazia (pedido pendente).
--   2. O ESP32 vem ver de 5 em 5 s se há pedidos pendentes (answered_at vazio).
--   3. Se houver, preenche distance_cm em todos os pendentes; o answered_at é
--      preenchido automaticamente pelo servidor (trigger abaixo).
--   4. O site recebe a atualização em direto e mostra o valor.
--
-- distance_cm = 999 significa "sem eco" (nenhuma leitura válida, nada até ~5 m).

create table distance_requests (
  id           bigint generated always as identity primary key,
  requested_at timestamptz not null default now(),
  answered_at  timestamptz,
  distance_cm  real
);

-- Hora da resposta definida pelo servidor (o ESP32 não tem relógio)
create function set_answered_at() returns trigger
language plpgsql set search_path = '' as $$
begin
  new.answered_at := now();
  return new;
end $$;

create trigger distance_requests_answered
  before update on distance_requests
  for each row execute function set_answered_at();

-- Row Level Security
alter table distance_requests enable row level security;

-- Site: só pode criar pedidos vazios
create policy "anon request" on distance_requests
  for insert to anon with check (answered_at is null and distance_cm is null);

-- Site e ESP32: podem ler
create policy "anon read" on distance_requests
  for select to anon using (true);

-- ESP32: só pode responder a pedidos ainda pendentes (uma resposta não é reescrita)
create policy "anon answer" on distance_requests
  for update to anon using (answered_at is null) with check (distance_cm is not null);

grant select, insert on distance_requests to anon;
grant update (distance_cm) on distance_requests to anon;

-- Realtime para o site receber a resposta sem recarregar
alter publication supabase_realtime add table distance_requests;
