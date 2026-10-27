-- ParSpoDec v2 — estado dos lugares L1 e L2
-- Corre isto no Supabase: SQL Editor → New query → colar tudo → Run
--
-- Códigos guardados em l1 / l2:
--   -1 = desconhecido (leitura inválida, ou L2 tapado pelo carro em L1)
--    0 = livre
--    1 = ocupado
--
-- O ESP32 só envia uma linha quando o estado muda, por isso cada linha
-- desta tabela é uma "mudança de estado" (serve também de histórico).

create table spot_states (
  id         bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  l1         smallint not null check (l1 in (-1, 0, 1)),
  l2         smallint not null check (l2 in (-1, 0, 1))
);

-- Row Level Security: a chave pública (anon/publishable) só pode inserir e ler
alter table spot_states enable row level security;

create policy "anon insert" on spot_states
  for insert to anon with check (true);

create policy "anon read" on spot_states
  for select to anon using (true);

grant select, insert on spot_states to anon;

-- Ativa o Realtime para a página atualizar sozinha
alter publication supabase_realtime add table spot_states;

-- (Opcional) A tabela antiga "readings" (distâncias) deixa de ser usada.
-- Se não quiseres guardar esses dados, podes apagá-la com:
--   drop table readings;
