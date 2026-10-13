CREATE TABLE IF NOT EXISTS person (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    person_no VARCHAR(64) NOT NULL UNIQUE,
    name VARCHAR(128) NOT NULL,
    enabled TINYINT NOT NULL DEFAULT 1,
    created_at DATETIME NOT NULL,
    updated_at DATETIME NOT NULL
);

CREATE TABLE IF NOT EXISTS face_feature (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    person_id BIGINT NOT NULL,
    image_path VARCHAR(512) NOT NULL,
    feature_blob LONGBLOB NOT NULL,
    feature_size INT NOT NULL,
    model_version VARCHAR(128),
    quality FLOAT DEFAULT 0,
    created_at DATETIME NOT NULL,
    FOREIGN KEY (person_id) REFERENCES person(id)
);

CREATE TABLE IF NOT EXISTS verify_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    person_id BIGINT,
    person_no VARCHAR(64),
    result VARCHAR(32) NOT NULL,
    cosine FLOAT DEFAULT 0,
    live_score FLOAT DEFAULT 0,
    snapshot_path VARCHAR(512),
    created_at DATETIME NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_task (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    task_type VARCHAR(64) NOT NULL,
    payload_json TEXT NOT NULL,
    sync_status VARCHAR(32) NOT NULL DEFAULT 'pending',
    retry_count INT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    updated_at DATETIME NOT NULL
);
