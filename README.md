# Rinha de Backend 2026 - C

Solução para a Rinha de Backend 2026.

## Stack

- API em C
- Load balancer em C
- Docker Compose
- Índice vetorial gerado no build da imagem

## Como rodar

```sh
docker compose up -d
```

A aplicação fica disponível na porta `9999`.

## Endpoints

- `GET /ready`
- `POST /fraud-score`

## Limites

O `docker-compose.yml` foi configurado para respeitar o limite da competição:

- 1 CPU total
- 350 MB RAM total
- 2 instâncias da API
- 1 load balancer
