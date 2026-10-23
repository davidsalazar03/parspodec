-- Corre isto no Supabase: SQL Editor → New query → Run

create table readings (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  distance_cm real not null
);

-- Row Level Security: a chave pública (anon/publishable) só pode inserir e ler
alter table readings enable row level security;

create policy "anon insert" on readings
  for insert to anon with check (distance_cm between 0 and 500);

create policy "anon read" on readings
  for select to anon using (true);

-- Ativa o Realtime para a dashboard atualizar sozinha
alter publication supabase_realtime add table readings;
